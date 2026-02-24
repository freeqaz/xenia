#!/usr/bin/env python3
from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


GUEST_ADDR_MIN = 0x80000000
GUEST_ADDR_MAX = 0xA0000000


def parse_int(text: str) -> int:
    s = text.strip()
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    base = 10
    if s.lower().startswith("0x"):
        base = 16
        s = s[2:]
    elif re.fullmatch(r"[0-9a-fA-F]+", s):
        base = 16
    if not s:
        raise ValueError("empty integer")
    v = int(s, base)
    return -v if neg else v


def fmt_addr(v: int | None) -> str:
    if v is None:
        return "<none>"
    return f"0x{v:08X}"


@dataclass
class Symbol:
    start: int
    end: int | None
    name: str
    source: str
    section: str | None = None


@dataclass
class SymbolIndex:
    syms: list[Symbol] = field(default_factory=list)
    starts: list[int] = field(default_factory=list)
    exact: dict[int, list[Symbol]] = field(default_factory=dict)

    def add(self, sym: Symbol) -> None:
        self.syms.append(sym)

    def finalize(self) -> None:
        self.syms.sort(key=lambda s: (s.start, 0 if s.end is not None else 1, s.name))
        self.starts = [s.start for s in self.syms]
        self.exact.clear()
        for s in self.syms:
            self.exact.setdefault(s.start, []).append(s)

    @staticmethod
    def _ok(sym: Symbol, prefer_text: bool) -> bool:
        if not prefer_text:
            return True
        return sym.section in (None, ".text")

    def exact_hits(self, addr: int, prefer_text: bool = False) -> list[Symbol]:
        hits = self.exact.get(addr, [])
        if not prefer_text:
            return hits
        preferred = [s for s in hits if self._ok(s, True)]
        return preferred or hits

    def containing(self, addr: int, prefer_text: bool = False) -> Symbol | None:
        i = bisect.bisect_right(self.starts, addr) - 1
        while i >= 0:
            s = self.syms[i]
            if s.start > addr:
                i -= 1
                continue
            if s.end is not None and s.start <= addr < s.end and self._ok(s, prefer_text):
                return s
            if s.start < addr and (addr - s.start) > 0x10000:
                break
            i -= 1
        return None

    def nearest_prev(self, addr: int, prefer_text: bool = False) -> Symbol | None:
        i = bisect.bisect_right(self.starts, addr) - 1
        while 0 <= i < len(self.syms):
            s = self.syms[i]
            if self._ok(s, prefer_text):
                return s
            i -= 1
        return None

    def lookup(self, addr: int, prefer_text: bool = False) -> str | None:
        c = self.containing(addr, prefer_text=prefer_text)
        if c:
            off = addr - c.start
            return f"{c.name}+0x{off:X}" if off else c.name
        ex = self.exact_hits(addr, prefer_text=prefer_text)
        if ex:
            return ex[0].name
        prev = self.nearest_prev(addr, prefer_text=prefer_text)
        if prev:
            off = addr - prev.start
            return f"{prev.name}+0x{off:X} (nearest)"
        return None


SYMBOLS_TXT_RE = re.compile(
    r"^(?P<name>.+?) = (?P<section>\.[^:]+):0x(?P<addr>[0-9A-Fa-f]+); .*?(?:size:0x(?P<size>[0-9A-Fa-f]+))?\b"
)
NM_WITH_SIZE_RE = re.compile(
    r"^(?P<addr>[0-9A-Fa-f]+)\s+(?P<size>[0-9A-Fa-f]+)\s+(?P<typ>[A-Za-z?])\s+(?P<name>.+)$"
)
NM_RE = re.compile(r"^(?P<addr>[0-9A-Fa-f]+)\s+(?P<typ>[A-Za-z?])\s+(?P<name>.+)$")
MAP_RE = re.compile(
    r"^\s*(?P<section>[0-9A-Fa-f]{4}):(?P<offset>[0-9A-Fa-f]{8})\s+"
    r"(?P<name>\S+)\s+(?P<addr>[0-9A-Fa-f]{8})\b"
)


def _add_symbol(idx: SymbolIndex, addr: int, size: int | None, name: str, source: str, section: str | None) -> None:
    if addr <= 0 or not name:
        return
    end = (addr + size) if (size is not None and size > 0) else None
    idx.add(Symbol(start=addr, end=end, name=name, source=source, section=section))


def load_symbol_file(path: Path, idx: SymbolIndex) -> tuple[int, str]:
    try:
        raw = path.read_text(encoding="utf-8", errors="ignore")
    except FileNotFoundError:
        return (0, f"{path}: not found")
    added = 0
    stripped = raw.lstrip()

    if stripped.startswith("{"):
        try:
            obj = json.loads(raw)
        except json.JSONDecodeError:
            obj = None
        if isinstance(obj, dict):
            targets = obj.get("targets")
            if isinstance(targets, dict):
                for key, val in targets.items():
                    if not isinstance(val, dict):
                        continue
                    addr_raw = (
                        val.get("guest_addr")
                        or val.get("guest_address")
                        or val.get("address")
                        or val.get("va")
                    )
                    if addr_raw is None:
                        continue
                    try:
                        addr = parse_int(str(addr_raw))
                    except ValueError:
                        continue
                    _add_symbol(idx, addr, None, str(key), str(path), None)
                    added += 1
                return (added, f"{path}: manifest targets")

    for line in raw.splitlines():
        s = line.strip()
        if not s:
            continue

        m = SYMBOLS_TXT_RE.match(s)
        if m:
            addr = int(m.group("addr"), 16)
            size = int(m.group("size"), 16) if m.group("size") else None
            _add_symbol(idx, addr, size, m.group("name"), str(path), m.group("section"))
            added += 1
            continue

        m = NM_WITH_SIZE_RE.match(s)
        if m:
            typ = m.group("typ")
            if typ.upper() == "U":
                continue
            _add_symbol(
                idx,
                int(m.group("addr"), 16),
                int(m.group("size"), 16),
                m.group("name"),
                str(path),
                None,
            )
            added += 1
            continue

        m = NM_RE.match(s)
        if m:
            typ = m.group("typ")
            if typ.upper() == "U":
                continue
            _add_symbol(idx, int(m.group("addr"), 16), None, m.group("name"), str(path), None)
            added += 1
            continue

        m = MAP_RE.match(line)
        if m:
            _add_symbol(
                idx,
                int(m.group("addr"), 16),
                None,
                m.group("name"),
                str(path),
                m.group("section"),
            )
            added += 1
            continue

    return (added, f"{path}: text/nm/map parse")


def load_symbols(paths: list[Path]) -> SymbolIndex | None:
    if not paths:
        return None
    idx = SymbolIndex()
    total = 0
    for p in paths:
        added, kind = load_symbol_file(p, idx)
        print(f"[symbols] {kind} added={added}")
        total += added
    if total == 0:
        return None
    idx.finalize()
    return idx


@dataclass
class Section:
    name: str
    vma: int
    vsize: int
    raw_off: int
    raw_size: int
    characteristics: int

    @property
    def is_executable(self) -> bool:
        return bool(self.characteristics & 0x20000000) or bool(self.characteristics & 0x20)

    def contains(self, addr: int) -> bool:
        size = max(self.vsize, self.raw_size)
        return self.vma <= addr < (self.vma + size)

    def contains_raw(self, addr: int) -> bool:
        return self.vma <= addr < (self.vma + self.raw_size)


class ImageReader:
    def describe_addr(self, addr: int) -> str:
        raise NotImplementedError

    def read_u32_be(self, addr: int) -> int | None:
        data = self.read(addr, 4)
        if data is None or len(data) != 4:
            return None
        return struct.unpack(">I", data)[0]

    def read(self, addr: int, size: int) -> bytes | None:
        raise NotImplementedError

    def is_executable_addr(self, addr: int) -> bool:
        return False

    @property
    def display_path(self) -> str:
        raise NotImplementedError


class PEImage(ImageReader):
    def __init__(self, path: Path, data: bytes):
        self.path = path
        self.data = data
        self.sections: list[Section] = []
        self.image_base = 0
        self._parse()

    @property
    def display_path(self) -> str:
        return str(self.path)

    def _parse(self) -> None:
        if self.data[:2] != b"MZ":
            raise ValueError("not a PE (missing MZ)")
        pe_off = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_off : pe_off + 4] != b"PE\x00\x00":
            raise ValueError("not a PE (missing PE signature)")
        coff_off = pe_off + 4
        (_machine, num_sections, _time, _ptr_sym, _num_sym, opt_size, _chars) = struct.unpack_from(
            "<HHIIIHH", self.data, coff_off
        )
        opt_off = coff_off + 20
        magic = struct.unpack_from("<H", self.data, opt_off)[0]
        if magic == 0x10B:
            self.image_base = struct.unpack_from("<I", self.data, opt_off + 28)[0]
        elif magic == 0x20B:
            self.image_base = struct.unpack_from("<Q", self.data, opt_off + 24)[0]
        else:
            raise ValueError(f"unsupported PE optional header magic 0x{magic:X}")
        sec_off = opt_off + opt_size
        for i in range(num_sections):
            off = sec_off + i * 40
            if off + 40 > len(self.data):
                break
            name_raw = self.data[off : off + 8]
            name = name_raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")
            vsize, vaddr, raw_size, raw_off = struct.unpack_from("<IIII", self.data, off + 8)
            characteristics = struct.unpack_from("<I", self.data, off + 36)[0]
            self.sections.append(
                Section(
                    name=name,
                    vma=int(self.image_base + vaddr),
                    vsize=vsize,
                    raw_off=raw_off,
                    raw_size=raw_size,
                    characteristics=characteristics,
                )
            )

    def section_for(self, addr: int) -> Section | None:
        for s in self.sections:
            if s.contains(addr):
                return s
        return None

    def describe_addr(self, addr: int) -> str:
        sec = self.section_for(addr)
        if not sec:
            return "outside-sections"
        off = addr - sec.vma
        flags = " exec" if sec.is_executable else ""
        return f"{sec.name}+0x{off:X}{flags}"

    def read(self, addr: int, size: int) -> bytes | None:
        sec = self.section_for(addr)
        if not sec:
            return None
        if not sec.contains_raw(addr):
            return None
        max_len = (sec.vma + sec.raw_size) - addr
        take = min(size, max_len)
        file_off = sec.raw_off + (addr - sec.vma)
        if file_off < 0 or (file_off + take) > len(self.data):
            return None
        return self.data[file_off : file_off + take]

    def is_executable_addr(self, addr: int) -> bool:
        sec = self.section_for(addr)
        return bool(sec and sec.is_executable)


class RawImage(ImageReader):
    def __init__(self, path: Path, data: bytes, base: int):
        self.path = path
        self.data = data
        self.base = base

    @property
    def display_path(self) -> str:
        return str(self.path)

    def describe_addr(self, addr: int) -> str:
        if self.base <= addr < self.base + len(self.data):
            return f"raw+0x{addr - self.base:X}"
        return "outside-raw"

    def read(self, addr: int, size: int) -> bytes | None:
        if addr < self.base or addr >= self.base + len(self.data):
            return None
        off = addr - self.base
        return self.data[off : off + size]

    def is_executable_addr(self, addr: int) -> bool:
        return self.base <= addr < self.base + len(self.data)


def decompress_xex_to_pe_bytes(xex_path: Path) -> bytes:
    data = xex_path.read_bytes()
    if data[:4] != b"XEX2":
        raise ValueError("not a XEX2 file")
    pe_offset = struct.unpack(">I", data[8:12])[0]
    opt_count = struct.unpack(">I", data[20:24])[0]

    bff_offset = None
    off = 24
    for _ in range(opt_count):
        if off + 8 > len(data):
            break
        hdr_id = struct.unpack(">I", data[off : off + 4])[0]
        hdr_val = struct.unpack(">I", data[off + 4 : off + 8])[0]
        if hdr_id == 0x000003FF:
            bff_offset = hdr_val
            break
        off += 8
    if bff_offset is None:
        raise ValueError("xex missing base file format header")

    if bff_offset + 8 > len(data):
        raise ValueError("xex truncated base file format descriptor")
    size = struct.unpack(">I", data[bff_offset : bff_offset + 4])[0]
    enc_type = struct.unpack(">H", data[bff_offset + 4 : bff_offset + 6])[0]
    comp_type = struct.unpack(">H", data[bff_offset + 6 : bff_offset + 8])[0]
    if enc_type != 0:
        raise ValueError(f"encrypted XEX unsupported (enc_type={enc_type})")

    if comp_type == 0:
        pe_data = data[pe_offset:]
    elif comp_type == 1:
        num_blocks = (size - 8) // 8
        out = bytearray()
        data_offset = pe_offset
        for i in range(num_blocks):
            block_off = bff_offset + 8 + i * 8
            if block_off + 8 > len(data):
                raise ValueError("xex truncated block table")
            blk_size = struct.unpack(">I", data[block_off : block_off + 4])[0]
            blk_zeros = struct.unpack(">I", data[block_off + 4 : block_off + 8])[0]
            end = data_offset + blk_size
            if end > len(data):
                raise ValueError("xex truncated block data")
            out.extend(data[data_offset:end])
            data_offset = end
            if blk_zeros:
                out.extend(b"\x00" * blk_zeros)
        pe_data = bytes(out)
    else:
        raise ValueError(f"unsupported XEX compression type {comp_type}")

    if pe_data[:2] != b"MZ":
        raise ValueError("decompressed XEX did not produce a PE (missing MZ)")
    return pe_data


@dataclass
class LoadedImage:
    reader: ImageReader
    disasm_path: Path
    disasm_kind: str  # pe/raw
    temp_path: Path | None = None
    temp_dir: tempfile.TemporaryDirectory[str] | None = None

    def cleanup(self) -> None:
        if self.temp_dir is not None:
            self.temp_dir.cleanup()


def load_image(image_path: Path, raw_base: int | None) -> LoadedImage:
    data = image_path.read_bytes()
    magic4 = data[:4]
    if magic4 == b"XEX2":
        pe_data = decompress_xex_to_pe_bytes(image_path)
        td = tempfile.TemporaryDirectory(prefix="dc3_guest_disasm_")
        pe_path = Path(td.name) / (image_path.stem + ".exe")
        pe_path.write_bytes(pe_data)
        reader = PEImage(pe_path, pe_data)
        return LoadedImage(reader=reader, disasm_path=pe_path, disasm_kind="pe", temp_path=pe_path, temp_dir=td)
    if data[:2] == b"MZ":
        reader = PEImage(image_path, data)
        return LoadedImage(reader=reader, disasm_path=image_path, disasm_kind="pe")
    if raw_base is None:
        raise ValueError(
            "unrecognized image format (expected XEX2 or PE/MZ). For extracted raw image, pass --raw-base."
        )
    reader = RawImage(image_path, data, raw_base)
    return LoadedImage(reader=reader, disasm_path=image_path, disasm_kind="raw")


DISASM_LINE_RE = re.compile(
    r"^\s*(?P<addr>[0-9A-Fa-f]+):\s+(?P<bytes>(?:[0-9A-Fa-f]{2}\s+){3}[0-9A-Fa-f]{2}|[0-9A-Fa-f]{8})\s+(?P<asm>.+?)\s*$"
)
HEX_IN_ASM_RE = re.compile(r"\b0x([0-9A-Fa-f]+)\b")


def parse_disasm_output(text: str) -> dict[int, str]:
    out: dict[int, str] = {}
    for line in text.splitlines():
        m = DISASM_LINE_RE.match(line)
        if not m:
            continue
        addr = int(m.group("addr"), 16)
        out[addr] = m.group("asm")
    return out


def run_disasm(
    img: LoadedImage,
    start: int,
    stop: int,
    llvm_objdump: str,
    gnu_objdump: str,
) -> dict[int, str]:
    if stop <= start:
        return {}
    if img.disasm_kind == "pe":
        cmd = [
            llvm_objdump,
            "--triple=powerpc64-unknown-windows",
            "-d",
            f"--start-address=0x{start:X}",
            f"--stop-address=0x{stop:X}",
            str(img.disasm_path),
        ]
    else:
        base = getattr(img.reader, "base", None)
        if base is None:
            return {}
        cmd = [
            gnu_objdump,
            "-D",
            "-b",
            "binary",
            "-m",
            "powerpc:common64",
            "-EB",
            f"--adjust-vma=0x{base:X}",
            f"--start-address=0x{start:X}",
            f"--stop-address=0x{stop:X}",
            str(img.disasm_path),
        ]
    try:
        cp = subprocess.run(cmd, check=False, capture_output=True, text=True)
    except FileNotFoundError:
        return {}
    if cp.returncode != 0 and not cp.stdout:
        return {}
    return parse_disasm_output(cp.stdout)


@dataclass
class CrashTuple:
    pc: int | None = None
    lr: int | None = None
    ctr: int | None = None


def extract_latest_crash_tuple(log_path: Path) -> CrashTuple:
    pc_re = re.compile(r"\bPC:\s*0x([0-9A-Fa-f]+)")
    lr_re = re.compile(r"\bGuest lr:\s*0x([0-9A-Fa-f]+)")
    ctr_re = re.compile(r"\bGuest ctr:\s*0x([0-9A-Fa-f]+)")
    cur = CrashTuple()
    last = CrashTuple()
    in_crash = False
    for line in log_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if "==== CRASH DUMP ====" in line:
            in_crash = True
            cur = CrashTuple()
            continue
        if not in_crash:
            continue
        if (m := pc_re.search(line)):
            cur.pc = int(m.group(1), 16)
        elif (m := lr_re.search(line)):
            cur.lr = int(m.group(1), 16)
        elif (m := ctr_re.search(line)):
            cur.ctr = int(m.group(1), 16)
        if cur.pc is not None and cur.lr is not None:
            last = CrashTuple(cur.pc, cur.lr, cur.ctr)
    if last.pc is None and last.lr is None and last.ctr is None:
        raise ValueError(f"no crash tuple found in {log_path}")
    return last


def add_target(rows: list[tuple[str, int]], label: str, value: int | None) -> None:
    if value is None:
        return
    rows.append((label, value))


def parse_labeled_addr(text: str) -> tuple[str, int]:
    if "=" in text:
        name, value = text.split("=", 1)
        return name.strip() or "addr", parse_int(value)
    return ("addr", parse_int(text))


def branch_annotation(asm_text: str, symbols: SymbolIndex | None) -> str:
    if not symbols:
        return ""
    m = HEX_IN_ASM_RE.search(asm_text)
    if not m:
        return ""
    target = int(m.group(1), 16)
    if not (GUEST_ADDR_MIN <= target < GUEST_ADDR_MAX):
        return ""
    sym = symbols.lookup(target, prefer_text=True)
    if not sym:
        return ""
    return f" ; -> {sym}"


def print_address_header(label: str, addr: int, img: ImageReader, symbols: SymbolIndex | None) -> None:
    desc = img.describe_addr(addr)
    prefer_text = img.is_executable_addr(addr)
    sym = symbols.lookup(addr, prefer_text=prefer_text) if symbols else None
    print(f"{label}: {fmt_addr(addr)}  [{desc}]")
    if sym:
        print(f"  symbol: {sym}")
    if symbols:
        exact = symbols.exact_hits(addr, prefer_text=prefer_text)
        if len(exact) > 1:
            print("  exact aliases: " + ", ".join(s.name for s in exact[:8]))


def print_window(
    label: str,
    addr: int,
    img: LoadedImage,
    symbols: SymbolIndex | None,
    before: int,
    after: int,
    llvm_objdump: str,
    gnu_objdump: str,
) -> None:
    center = addr & ~0x3
    start = max(0, center - before * 4)
    stop = center + (after + 1) * 4
    disasm = run_disasm(img, start, stop, llvm_objdump=llvm_objdump, gnu_objdump=gnu_objdump)
    print()
    print(f"{label} window ({before} before / {after} after):")
    for a in range(start, stop, 4):
        mark = "<--" if a == center else "   "
        word = img.reader.read_u32_be(a)
        if word is None:
            print(f" {mark} {fmt_addr(a)}: <unmapped>")
            continue
        asm_text = disasm.get(a)
        line = f" {mark} {fmt_addr(a)}: {word:08X}"
        if asm_text:
            line += f"  {asm_text}"
            line += branch_annotation(asm_text, symbols)
        if a == center and not img.reader.is_executable_addr(a):
            line += "  [non-executable section]"
        print(line)


def ensure_tool_exists(name_or_path: str) -> str:
    if os.path.sep in name_or_path:
        p = Path(name_or_path)
        if p.exists():
            return str(p)
    from shutil import which

    found = which(name_or_path)
    if found:
        return found
    return name_or_path


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Symbolize and disassemble PPC guest code around DC3/Xenia crash addresses (PC/LR/CTR)."
    )
    ap.add_argument("--image", required=True, help="Guest image path: XEX2, Xbox PE (default.exe), or raw PPC blob")
    ap.add_argument("--raw-base", help="Base guest address for raw extracted image (required for non-XEX/non-PE blobs)")
    ap.add_argument("--symbols", action="append", default=[], help="Symbol source file(s): symbols.txt, nm output, map, or manifest JSON")
    ap.add_argument("--pc", help="Guest PC address")
    ap.add_argument("--lr", help="Guest LR address")
    ap.add_argument("--ctr", help="Guest CTR address (optional)")
    ap.add_argument("--addr", action="append", default=[], help="Extra labeled address (`name=0xADDR`) or plain address")
    ap.add_argument("--xenia-log", help="Parse latest crash tuple from xenia-headless log and use PC/LR/CTR")
    ap.add_argument("--before", type=int, default=12, help="Instructions before each anchor to print")
    ap.add_argument("--after", type=int, default=12, help="Instructions after each anchor to print")
    ap.add_argument("--no-dedup", action="store_true", help="Do not deduplicate repeated addresses (PC/LR same window, etc.)")
    ap.add_argument("--llvm-objdump", default="llvm-objdump", help="Path to llvm-objdump (used for PE/XEX)")
    ap.add_argument(
        "--gnu-objdump",
        default="powerpc-none-eabi-objdump",
        help="Path to GNU objdump with PPC support (used for raw binaries)",
    )
    args = ap.parse_args()

    crash = CrashTuple()
    if args.xenia_log:
        crash = extract_latest_crash_tuple(Path(args.xenia_log))
        print(
            f"[log] latest crash tuple from {args.xenia_log}: "
            f"PC={fmt_addr(crash.pc)} LR={fmt_addr(crash.lr)} CTR={fmt_addr(crash.ctr)}"
        )

    pc = parse_int(args.pc) if args.pc else crash.pc
    lr = parse_int(args.lr) if args.lr else crash.lr
    ctr = parse_int(args.ctr) if args.ctr else crash.ctr

    targets: list[tuple[str, int]] = []
    add_target(targets, "PC", pc)
    add_target(targets, "LR", lr)
    add_target(targets, "CTR", ctr)
    for item in args.addr:
        name, val = parse_labeled_addr(item)
        add_target(targets, name, val)

    if not targets:
        print("error: no addresses specified (use --pc/--lr/--ctr or --xenia-log)", file=sys.stderr)
        return 2

    raw_base = parse_int(args.raw_base) if args.raw_base else None
    llvm_objdump = ensure_tool_exists(args.llvm_objdump)
    gnu_objdump = ensure_tool_exists(args.gnu_objdump)
    symbols = load_symbols([Path(p) for p in args.symbols]) if args.symbols else None

    loaded: LoadedImage | None = None
    try:
        loaded = load_image(Path(args.image), raw_base)
        print(f"[image] {loaded.reader.display_path}")
        if loaded.temp_path:
            print(f"[image] decompressed XEX -> {loaded.temp_path}")

        # Print a compact summary before windows.
        print()
        print("Anchors:")
        seen: set[int] = set()
        ordered: list[tuple[str, int]] = []
        for label, addr in targets:
            if not args.no_dedup and addr in seen:
                print(f"{label}: {fmt_addr(addr)}  [duplicate]")
                continue
            seen.add(addr)
            ordered.append((label, addr))
            print_address_header(label, addr, loaded.reader, symbols)

        for label, addr in ordered:
            if addr == 0:
                print()
                print(f"{label} window: skipped disassembly for null address")
                continue
            print_window(
                label,
                addr,
                loaded,
                symbols,
                before=max(0, args.before),
                after=max(0, args.after),
                llvm_objdump=llvm_objdump,
                gnu_objdump=gnu_objdump,
            )
    finally:
        if loaded is not None:
            loaded.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
