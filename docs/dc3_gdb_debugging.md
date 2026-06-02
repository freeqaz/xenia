# DC3 Guest (PowerPC) Debugging with the Headless GDB-RSP Stub

This documents real interactive debugging of Dance Central 3's guest PowerPC
code running under `xenia-headless` on Linux: breakpoints, pause, single-step,
register and memory reads — over the GDB Remote Serial Protocol (RSP).

## TL;DR

```bash
# 1. Build (Checked, Linux):
cd build && make xenia-headless config=checked_linux -j$(nproc)

# 2. Launch DC3 with the RSP stub on GPU 0:
./build/bin/Linux/Checked/xenia-headless \
  --target=/path/to/debug.xex --gpu=vulkan --vulkan_device=0 \
  --dc3_nui_patch_layout=original --dc3_crt_skip_nui=true --fake_kinect_data=true \
  --dc3_gdb_rsp_stub=true --dc3_gdb_rsp_port=9001 \
  --dc3_gdb_rsp_break_on_connect=true \
  --headless_timeout_ms=300000

# 3a. Recommended client: the Python RSP helper (robust, scriptable).
#     See "Python RSP client" below.

# 3b. Host gdb 17.2: ALWAYS set the architecture/endianness FIRST, or gdb
#     core-dumps on connect (Xbox 360 = 64-bit big-endian PowerPC):
gdb -ix /path/to/xenia/docs/dc3.gdbinit
#  (.gdbinit content is below)
```

## What works

| Capability                          | Status |
|-------------------------------------|--------|
| Memory reads (`m`)                  | yes (now page-protection validated) |
| Register reads (`g`, `p`)           | yes (GPR0-31, pc, msr, cr, lr, ctr, xer, fpscr) |
| Software breakpoints (`Z0`/`z0`)    | yes  |
| Pause / interrupt                   | yes (`break_on_connect` or RSP `\x03`) |
| Continue (`c`)                      | yes  |
| Single-step (`s`)                   | yes (guest-instruction granularity) |
| Thread list (`qfThreadInfo`)        | yes  |
| Backtrace                           | partial (frame[0] always exact; rbp-walk best-effort) |

Proven end-to-end: breakpoint at `DxRnd::Handle` (0x82611488), hit on the main
game thread (thread 6), register + memory read, two single-steps
(0x82611488 -> 0x8261148c -> stepped into callee), then `continue` re-hit the
same breakpoint.

## Root cause that was fixed

The stub gated all pause/step/breakpoint operations behind
`processor_->stack_walker() != nullptr`. On Linux the POSIX stack walker
(`src/xenia/cpu/stack_walker_posix.cc`) was a stub that always returned
`nullptr`, so the gate was permanently closed — the stub logged *"stack walker
unavailable ... live pause/step/breakpoint debugging is disabled"*. This was
true for BOTH the headless and the full (`xenia-app`) builds on Linux; the full
build's debugger is equally disabled on Linux for the same reason. (The
windowed build is also unusable here because `DISPLAY` is unset.)

Xenia's breakpoint mechanism patches a `UD2` (0x0F0B) into the host JIT machine
code; the resulting SIGILL is caught and routed to
`Processor::OnThreadBreakpointHit`. None of that needs a stack walker. The
stack walker is only used by `Processor::UpdateThreadExecutionStates()` to (a)
resolve a host PC to a guest PC and (b) produce backtrace frames. The fix
implements a real POSIX stack walker:

- `ResolveStack()` maps host PC -> guest function/PC via the JIT code cache
  (identical to the Win32 walker). Frame[0] (the exact faulting/suspended PC)
  is mapped *without* the `-1` return-address adjustment so breakpoint matching
  is exact.
- `CaptureStackTrace(context)` walks the x86-64 `rbp` frame-pointer back-chain
  from the supplied host context. Frame[0] is always exact (taken straight from
  the exception/suspend context); deeper frames are best-effort.

## Host gdb 17.2 caveat (IMPORTANT)

gdb 17.2 (x86-64) has PowerPC support but **SIGSEGVs on `target remote`** unless
you select the architecture before connecting:

```
set architecture powerpc:common64
set endian big
```

Additionally, the stub's target description deliberately **omits an
`<architecture>` element** — serving a PPC `<architecture>` together with the
standard `org.gnu.gdb.power.core` feature reliably crashes gdb 17.2 inside its
rs6000 target-dependent code. With the element omitted, the client supplies the
architecture (above) and gdb accepts the register feature cleanly.

NOTE: even with these settings, gdb 17.2 may still crash in its PPC frame
unwinder when attaching to a *paused, multi-threaded* target with no symbol
file loaded (a host-gdb bug in the prologue analyzer, not a stub bug). The
robust, fully-validated client is the Python RSP helper below. If you use gdb,
prefer attaching to a *running* target and letting a breakpoint stop it.

### docs/dc3.gdbinit

```gdb
set pagination off
set confirm off
set architecture powerpc:common64
set endian big
# Optional: load the retail symbol map as a bare symbol file for nicer names.
# (Addresses are valid for debug.xex.)
target remote 127.0.0.1:9001
```

## Python RSP client (recommended)

A minimal, robust client lives at `docs/dc3_rsp_client.py` (copy below). It
speaks RSP directly and is immune to the gdb-client crash.

```python
import socket
class RSP:
    def __init__(self, host="127.0.0.1", port=9001, timeout=30):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout); self.ack = True
    def _cks(self, p): return sum(p.encode()) & 0xff
    def send(self, p):
        self.s.sendall(("$%s#%02x" % (p, self._cks(p))).encode())
        if self.ack: self.s.recv(1)
    def recv(self):
        while self.s.recv(1) != b"$": pass
        b = b""
        while True:
            c = self.s.recv(1)
            if c == b"#": break
            b += c
        self.s.recv(2)
        if self.ack: self.s.sendall(b"+")
        return b.decode()
    def cmd(self, p): self.send(p); return self.recv()
    def regs(self):
        g = self.cmd("g")
        return {i: g[i*8:i*8+8] for i in range(39)}  # 0-31 GPR, 32 pc, 35 lr ...

r = RSP(port=9001)
r.cmd("qSupported:swbreak+")
r.send("QStartNoAckMode"); r.recv(); r.ack = False
r.cmd("Hg0"); r.cmd("?")
r.cmd("Z0,82611488,4")          # set breakpoint
print("stop:", r.cmd("c"))      # continue until hit
g = r.regs()
print("PC =", g[32], "LR =", g[35])
print("step:", r.cmd("s")); print("PC =", r.regs()[32])
r.cmd("z0,82611488,4"); r.cmd("D")
```

Register numbering in the `g`/`p` packets: 0-31 = r0-r31, 32 = pc, 33 = msr,
34 = cr, 35 = lr, 36 = ctr, 37 = xer, 38 = fpscr (all 32-bit, big-endian hex).

## Picking a breakpoint that fires

A breakpoint only fires when the guest actually executes that address. In the
attract/boot render loop, good always-running targets are render-thread
functions such as `DxRnd::Handle` (0x82611488). To find live targets, watch the
periodic "Thread Status Report" in the xenia log — thread 6 is the main game
thread; its LR shows what guest code is currently executing. Resolve an address
to a symbol with the retail map
(`dc3-decomp/orig/373307D9/ham_xbox_r.map`).

## Run flags

| Flag | Purpose |
|------|---------|
| `--dc3_gdb_rsp_stub=true`            | enable the in-process RSP stub |
| `--dc3_gdb_rsp_port=9001`            | listen port (host is `127.0.0.1`) |
| `--dc3_gdb_rsp_break_on_connect=true`| pause the target when a client connects |

## Single-client limitation & reconnect

The stub serves one client at a time. The accepted socket now has TCP keepalive
enabled, and if a client vanishes while the target is paused the stub
auto-continues the target and clears breakpoints so a fresh client can attach to
a live target. A continue/step that is blocked waiting for a breakpoint to fire
will, however, hold the slot until the breakpoint hits or the process exits — if
a client dies during a blocked `continue`, restart `xenia-headless`.

## Files changed

- `src/xenia/cpu/stack_walker_posix.cc` — real POSIX (Linux/x86-64) stack walker.
- `src/xenia/app/emulator_headless.cc` — target.xml omits `<architecture>`;
  memory reads validate guest page protection; TCP keepalive + auto-continue on
  client loss.
