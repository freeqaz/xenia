#!/usr/bin/env python3
from __future__ import annotations

import argparse
import binascii
import json
import socket
import sys
from dataclasses import dataclass, field
from pathlib import Path
from datetime import datetime, timezone

import dc3_crash_signature_triage as triage
import dc3_guest_disasm as guest_disasm


def _u32(v: int | None) -> int:
    return (v or 0) & 0xFFFFFFFF


def _be32_hex(v: int) -> str:
    return v.to_bytes(4, "big").hex()


def _checksum(data: bytes) -> bytes:
    return f"{(sum(data) & 0xFF):02x}".encode("ascii")


def _frame(payload: bytes) -> bytes:
    return b"$" + payload + b"#" + _checksum(payload)


@dataclass
class Snapshot:
    regs: dict[str, int]
    pc: int
    lr: int
    ctr: int
    cr: int = 0
    xer: int = 0
    msr: int = 0
    fpscr: int = 0

    def _reg_order(self) -> list[tuple[str, int]]:
        regs: list[tuple[str, int]] = []
        for i in range(32):
            regs.append((f"r{i}", _u32(self.regs.get(f"r{i}"))))
        regs += [
            ("pc", _u32(self.pc)),
            ("msr", _u32(self.msr)),
            ("cr", _u32(self.cr)),
            ("lr", _u32(self.lr)),
            ("ctr", _u32(self.ctr)),
            ("xer", _u32(self.xer)),
            ("fpscr", _u32(self.fpscr)),
        ]
        return regs

    def reg_count(self) -> int:
        return len(self._reg_order())

    def get_reg(self, index: int) -> int | None:
        order = self._reg_order()
        if 0 <= index < len(order):
            return order[index][1]
        return None

    def set_reg(self, index: int, value: int) -> bool:
        order = self._reg_order()
        if index < 0 or index >= len(order):
            return False
        name = order[index][0]
        value = _u32(value)
        if name.startswith("r"):
            self.regs[name] = value
        else:
            setattr(self, name, value)
        return True

    def g_packet(self) -> bytes:
        # Minimal PowerPC register set encoding for GDB compatibility experiments:
        # gpr0..gpr31, pc, msr, cr, lr, ctr, xer, fpscr (all 32-bit big-endian).
        fields: list[int] = [value for (_name, value) in self._reg_order()]
        return "".join(_be32_hex(v) for v in fields).encode("ascii")


@dataclass
class ServerState:
    snapshot: Snapshot
    image: guest_disasm.LoadedImage
    breakpoints: set[int] = field(default_factory=set)
    no_ack_mode: bool = False
    last_signal: bytes = b"S05"  # SIGTRAP
    target_xml: bytes = b""
    packet_trace_path: str | None = None
    packet_trace_fp: object | None = None

    def read_mem(self, addr: int, size: int) -> bytes | None:
        data = self.image.reader.read(addr, size)
        if data is None or len(data) != size:
            return None
        return data

    def trace(self, direction: str, payload: bytes) -> None:
        if not self.packet_trace_fp:
            return
        ts = datetime.now(timezone.utc).isoformat()
        text = payload.decode("ascii", errors="replace")
        self.packet_trace_fp.write(f"{ts} {direction} {text}\n")
        self.packet_trace_fp.flush()


def parse_rsp_packet(stream: socket.socket) -> bytes | None:
    # Wait for '$', handle standalone ACKs/noise.
    while True:
        b = stream.recv(1)
        if not b:
            return None
        if b == b"$":
            break
        if b in (b"+", b"-"):
            continue
    payload = bytearray()
    while True:
        b = stream.recv(1)
        if not b:
            return None
        if b == b"#":
            break
        payload.extend(b)
    cksum = stream.recv(2)
    if len(cksum) < 2:
        return None
    try:
        got = int(cksum.decode("ascii"), 16)
    except ValueError:
        return None
    expect = sum(payload) & 0xFF
    if got != expect:
        return b"__BAD_CHECKSUM__"
    return bytes(payload)


def send_rsp(stream: socket.socket, payload: bytes) -> None:
    stream.sendall(_frame(payload))


def handle_packet(state: ServerState, pkt: bytes) -> bytes:
    text = pkt.decode("ascii", errors="ignore")

    if text == "?":
        return state.last_signal
    if text == "qSupported":
        return b"PacketSize=4000;swbreak+;QStartNoAckMode+;qXfer:features:read+"
    if text.startswith("qSupported:"):
        return b"PacketSize=4000;swbreak+;QStartNoAckMode+;qXfer:features:read+"
    if text == "QStartNoAckMode":
        state.no_ack_mode = True
        return b"OK"
    if text == "qAttached":
        return b"1"
    if text in ("qfThreadInfo", "qfThreadInfo:"):
        return b"m1"
    if text == "qsThreadInfo":
        return b"l"
    if text.startswith("Hg") or text.startswith("Hc"):
        return b"OK"
    if text == "g":
        return state.snapshot.g_packet()
    if text.startswith("p"):
        try:
            regno = int(text[1:], 16)
        except Exception:
            return b"E10"
        value = state.snapshot.get_reg(regno)
        if value is None:
            return b"E11"
        return _be32_hex(value).encode("ascii")
    if text.startswith("P"):
        try:
            lhs, rhs = text[1:].split("=", 1)
            regno = int(lhs, 16)
            value = int(rhs, 16)
        except Exception:
            return b"E12"
        return b"OK" if state.snapshot.set_reg(regno, value) else b"E13"
    if text.startswith("m"):
        try:
            addr_len = text[1:]
            addr_s, len_s = addr_len.split(",", 1)
            addr = int(addr_s, 16)
            length = int(len_s, 16)
        except Exception:
            return b"E01"
        data = state.read_mem(addr, length)
        if data is None:
            return b"E02"
        return binascii.hexlify(data)
    if text.startswith("Z0,") or text.startswith("z0,"):
        try:
            kind, rest = text[0], text[3:]
            addr_s, _size_s = rest.split(",", 1)
            addr = int(addr_s, 16)
            if kind == "Z":
                state.breakpoints.add(addr)
            else:
                state.breakpoints.discard(addr)
            return b"OK"
        except Exception:
            return b"E03"
    if text.startswith("c") or text.startswith("s"):
        # Snapshot server doesn't execute; it immediately reports same stop.
        return state.last_signal
    if text.startswith("qXfer:features:read:target.xml:"):
        try:
            suffix = text.split("target.xml:", 1)[1]
            off_s, len_s = suffix.split(",", 1)
            off = int(off_s, 16)
            length = int(len_s, 16)
        except Exception:
            return b"E20"
        blob = state.target_xml
        if off >= len(blob):
            return b"l"
        chunk = blob[off : off + length]
        more = (off + length) < len(blob)
        return (b"m" if more else b"l") + chunk
    if text.startswith("T"):
        return b"OK"
    if text.startswith("qOffsets"):
        return b"Text=0;Data=0;Bss=0"
    if text.startswith("qC"):
        return b"QC1"
    if text == "qTStatus":
        return b""
    if text.startswith("qSymbol"):
        return b"OK"
    if text == "QThreadSuffixSupported":
        return b"OK"
    if text == "vMustReplyEmpty":
        return b""
    if text == "vCont?":
        return b"vCont;c;s"
    if text.startswith("vCont;"):
        return state.last_signal
    if text in ("k", "D"):
        return b"OK"
    # Unsupported packet: empty response is standard.
    return b""


def serve_once(host: str, port: int, state: ServerState) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, port))
        srv.listen(1)
        print(f"[rsp] listening on {host}:{port}")
        conn, addr = srv.accept()
        with conn:
            print(f"[rsp] client connected: {addr[0]}:{addr[1]}")
            while True:
                pkt = parse_rsp_packet(conn)
                if pkt is None:
                    print("[rsp] client disconnected")
                    return
                if pkt == b"__BAD_CHECKSUM__":
                    if not state.no_ack_mode:
                        conn.sendall(b"-")
                        state.trace("tx-ack", b"-")
                    continue
                if not state.no_ack_mode:
                    conn.sendall(b"+")
                    state.trace("tx-ack", b"+")
                state.trace("rx", pkt)
                resp = handle_packet(state, pkt)
                send_rsp(conn, resp)
                state.trace("tx", resp)
                if pkt in (b"k", b"D"):
                    print("[rsp] detach/kill requested; closing")
                    return


def build_snapshot(log_path: Path) -> Snapshot:
    crash = triage.parse_latest_crash_dump(log_path)
    if crash is None or crash.pc is None:
        raise ValueError(f"no crash dump with PC found in {log_path}")
    return Snapshot(
        regs=crash.regs,
        pc=_u32(crash.pc),
        lr=_u32(crash.lr),
        ctr=_u32(crash.ctr),
    )


def build_snapshot_from_json(path: Path) -> Snapshot:
    obj = json.loads(path.read_text(encoding="utf-8"))
    gpr_raw = obj.get("gpr", [])
    regs: dict[str, int] = {}
    if isinstance(gpr_raw, list):
        for i, v in enumerate(gpr_raw[:32]):
            if isinstance(v, int):
                regs[f"r{i}"] = _u32(v)
    guest_xer = obj.get("guest_xer", {}) if isinstance(obj.get("guest_xer"), dict) else {}
    xer = 0
    # Pack CA/OV/SO into the common XER low bits used by debuggers (approximate).
    if int(guest_xer.get("ca", 0) or 0):
        xer |= 1 << 29
    if int(guest_xer.get("ov", 0) or 0):
        xer |= 1 << 30
    if int(guest_xer.get("so", 0) or 0):
        xer |= 1 << 31
    return Snapshot(
        regs=regs,
        pc=_u32(int(obj.get("guest_pc", 0) or 0)),
        lr=_u32(int(obj.get("guest_lr", 0) or 0)),
        ctr=_u32(int(obj.get("guest_ctr", 0) or 0)),
        cr=_u32(int(obj.get("guest_cr", 0) or 0)),
        xer=_u32(xer),
    )


def build_target_xml() -> bytes:
    reg_lines: list[str] = []
    regnum = 0
    for i in range(32):
        reg_lines.append(
            f'    <reg name="r{i}" bitsize="32" regnum="{regnum}" type="uint32"/>\n'
        )
        regnum += 1
    for name in ("pc", "msr", "cr", "lr", "ctr", "xer", "fpscr"):
        reg_lines.append(
            f'    <reg name="{name}" bitsize="32" regnum="{regnum}" type="uint32"/>\n'
        )
        regnum += 1
    xml = (
        '<?xml version="1.0"?>\n'
        '<!DOCTYPE target SYSTEM "gdb-target.dtd">\n'
        '<target version="1.0">\n'
        '  <architecture>powerpc:common</architecture>\n'
        '  <feature name="org.gnu.gdb.power.core">\n'
        + "".join(reg_lines)
        + "  </feature>\n"
        "</target>\n"
    )
    return xml.encode("utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Standalone GDB RSP MVP mock server backed by a Xenia crash snapshot (Phase 4 protocol groundwork)."
    )
    src_group = ap.add_mutually_exclusive_group(required=True)
    src_group.add_argument("--log", help="Xenia headless log with a crash dump")
    src_group.add_argument(
        "--snapshot-json",
        help="Structured crash snapshot JSON (from Xenia --dc3_crash_snapshot_path)",
    )
    ap.add_argument("--image", required=True, help="Guest image (XEX/PE/raw) for memory reads")
    ap.add_argument("--raw-base", help="Base guest address for raw images")
    ap.add_argument("--host", default="127.0.0.1", help="Listen host")
    ap.add_argument("--port", type=int, default=9001, help="Listen port")
    ap.add_argument("--packet-trace", help="Optional text file to log RSP RX/TX payloads")
    args = ap.parse_args()

    raw_base = guest_disasm.parse_int(args.raw_base) if args.raw_base else None
    loaded = guest_disasm.load_image(Path(args.image), raw_base)
    try:
        if args.snapshot_json:
            snap = build_snapshot_from_json(Path(args.snapshot_json))
        else:
            snap = build_snapshot(Path(args.log))
        state = ServerState(snapshot=snap, image=loaded, target_xml=build_target_xml())
        if args.packet_trace:
            state.packet_trace_path = args.packet_trace
            state.packet_trace_fp = open(args.packet_trace, "w", encoding="utf-8")
        print(
            f"[rsp] snapshot PC={guest_disasm.fmt_addr(snap.pc)} "
            f"LR={guest_disasm.fmt_addr(snap.lr)} CTR={guest_disasm.fmt_addr(snap.ctr)}"
        )
        print(f"[rsp] registers={snap.reg_count()} target.xml_bytes={len(state.target_xml)}")
        if state.packet_trace_path:
            print(f"[rsp] packet_trace={state.packet_trace_path}")
        serve_once(args.host, args.port, state)
    finally:
        try:
            if 'state' in locals() and state.packet_trace_fp:
                state.packet_trace_fp.close()
        except Exception:
            pass
        loaded.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
