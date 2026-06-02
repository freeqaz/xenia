#!/usr/bin/env python3
"""Minimal, robust GDB-RSP client for the DC3 xenia-headless debug stub.

This speaks the GDB Remote Serial Protocol directly and is immune to the host
gdb 17.2 frame-unwinder crash. Use it to set guest PowerPC breakpoints, read
registers/memory, single-step, and continue.

Register numbering (g/p packets, 32-bit big-endian hex):
  0-31 = r0..r31, 32 = pc, 33 = msr, 34 = cr, 35 = lr, 36 = ctr,
  37 = xer, 38 = fpscr

Example:
  python3 dc3_rsp_client.py --port 9001 --break 0x82611488
"""
import argparse
import socket
import sys

REG_NAMES = {32: "pc", 33: "msr", 34: "cr", 35: "lr", 36: "ctr",
             37: "xer", 38: "fpscr"}


class RSP:
    def __init__(self, host="127.0.0.1", port=9001, timeout=30):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.ack = True

    def _cks(self, p):
        return sum(p.encode()) & 0xFF

    def send(self, payload):
        self.s.sendall(("$%s#%02x" % (payload, self._cks(payload))).encode())
        if self.ack:
            self.s.recv(1)  # consume '+'

    def recv(self):
        while True:
            c = self.s.recv(1)
            if not c:
                return None
            if c == b"$":
                break
        buf = b""
        while True:
            c = self.s.recv(1)
            if c == b"#":
                break
            buf += c
        self.s.recv(2)  # checksum
        if self.ack:
            self.s.sendall(b"+")
        return buf.decode(errors="replace")

    def cmd(self, payload):
        self.send(payload)
        return self.recv()

    def handshake(self):
        self.cmd("qSupported:swbreak+")
        self.send("QStartNoAckMode")
        self.recv()
        self.ack = False
        self.cmd("Hg0")
        return self.cmd("?")

    def regs(self):
        g = self.cmd("g")
        if not g or g.startswith("E"):
            return None
        return {i: int(g[i * 8:i * 8 + 8], 16) for i in range(len(g) // 8)}

    def read_mem(self, addr, length):
        return self.cmd("m%x,%x" % (addr, length))

    def set_bp(self, addr):
        return self.cmd("Z0,%x,4" % addr)

    def clear_bp(self, addr):
        return self.cmd("z0,%x,4" % addr)

    def cont(self):
        return self.cmd("c")

    def step(self):
        return self.cmd("s")

    def detach(self):
        return self.cmd("D")


def dump_regs(r):
    g = r.regs()
    if not g:
        print("  <registers unavailable>")
        return
    print("  pc=%08x lr=%08x ctr=%08x cr=%08x xer=%08x" %
          (g[32], g[35], g[36], g[34], g[37]))
    for row in range(0, 32, 4):
        print("  " + " ".join("r%-2d=%08x" % (i, g[i])
                              for i in range(row, row + 4)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9001)
    ap.add_argument("--break", dest="bp", type=lambda x: int(x, 0),
                    help="guest address to break at, e.g. 0x82611488")
    ap.add_argument("--steps", type=int, default=2,
                    help="single-steps to take after the hit")
    args = ap.parse_args()

    r = RSP(args.host, args.port)
    print("connected; stop reason:", r.handshake())

    if args.bp is None:
        dump_regs(r)
        r.detach()
        return

    print("setting breakpoint @ 0x%08x ->" % args.bp, r.set_bp(args.bp))
    print("continue -> stop reason:", r.cont())
    dump_regs(r)
    print("memory @ pc:", r.read_mem(args.bp, 16))
    for _ in range(args.steps):
        print("step -> stop reason:", r.step())
        g = r.regs()
        print("  pc=%08x" % (g[32] if g else 0))
    r.clear_bp(args.bp)
    r.detach()
    print("detached")


if __name__ == "__main__":
    sys.exit(main())
