# 02 — Guest→Host Address Translation (and what `last_fault=0x100000000` actually means)

All citations are xenia source on branch `headless-vulkan-linux`.
See [00-source-map.md](00-source-map.md) for file roles.

## ⚠ Headline: the "32-bit overflow" hypothesis is probably WRONG

Two load-bearing facts, established below:

1. **Every normal JIT load/store truncates the guest EA to 32 bits** before
   adding membase (`mov eax, reg.cvt32()` zero-extends on x86-64) — a garbage
   64-bit GPR value *cannot* push a normal access past `membase + 0xFFFFFFFF`.
2. **The host mapping base is chosen by probing `1ull << n` for n = 32..63**
   (`memory.cc:144-151`). On Linux the first probe (n=32) virtually always
   succeeds, so **`virtual_membase_ == 0x100000000` exactly**.

Therefore a raw fault address of `0x100000000` printed by the headless harness
(`emulator_headless.cc:1036-1051`, which prints `last_fault` as a **raw host
pointer**, never subtracting membase) decodes to:

```
guest_VA = 0x100000000 - virtual_membase_ = 0x0
```

**→ a guest NULL-pointer dereference**, which faults because the first 64 KiB
of the guest virtual heap is guard-protected (`protect_zero`,
`memory.cc:182-187`). The recursive string-keyed map lookup at RB3's crash PC
walking into a NULL node fits this exactly. The investigation should pivot
from "JIT address-wrap bug" to "**why does the guest compute NULL here?**"
(kernel/XAM/file-I/O divergence feeding DTA parse) — see
[03-guest-code-analysis.md](03-guest-code-analysis.md) and
[06-root-cause.md](06-root-cause.md).

## The membase scheme (mechanics)

- Pinned host registers for all JITted code: `rsi` = PPCContext
  (`x64_emitter.cc:921`), `rdi` = `virtual_membase_` (`x64_emitter.cc:922`),
  loaded in the prologue (`x64_emitter.cc:310-312`) and re-loaded after
  clobbering calls via `ReloadMembase()` (`x64_emitter.cc:928-929`).
- All `OPCODE_LOAD`/`OPCODE_STORE` variants compute
  `host = rdi + zeroext32(guest_EA) [+ const]` via
  `ComputeMemoryAddress`/`ComputeMemoryAddressOffset`
  (`x64_seq_memory.cc:31-104`).

## Truncation semantics

- PPC GPRs are modeled as full `uint64_t` (`ppc_context.h:259`). EA math in
  the frontend is a plain 64-bit `Add` (`ppc_emit_memory.cc:26-48`), and
  update-form EAs are stored back **unmasked** (`ppc_emit_memory.cc:50-54`,
  which contains a commented-out truncate). So high garbage bits *can* live in
  GPRs…
- …but truncation happens at codegen for every register-address access:
  `e.mov(e.eax, guest.reg().cvt32())` (`x64_seq_memory.cc:63,100`) — 32-bit
  mov zero-extends into rax. Constant addresses fold through
  `static_cast<uint32_t>` (`x64_seq_memory.cc:38,76`).
- **The one non-truncating path is dead code**: `EmitAtomicExchangeXX`
  (`OPCODE_ATOMIC_EXCHANGE`, `x64_seq_memory.cc:106-166`) uses the operand as
  a raw HOST pointer (self-flagged "This is weird, and should be fixed") — but
  `HIRBuilder::AtomicExchange()` has **zero callers**. `ATOMIC_COMPARE_EXCHANGE`
  truncates correctly (`x64_seq_memory.cc:171-214`).

## What lives at membase + 4 GiB

Not a guard region: `physical_membase_ = mapping_base_ + 0x100000000`
(`memory.cc:157-158`) — the guest **physical** heap (512 MiB) inside one
~4.5 GiB file-backed reservation (`memory.cc:127-214`). Guard pages exist only
at guest 0x0 (first 64 KiB, `protect_zero`) and the physical heap's tail
(`memory.cc:188-189`). Page 0 of physical is normally committed, so even a
hypothetical non-truncated access at membase+4 GiB would usually alias
silently, *not* fault — further evidence the observed fault is the
guest-NULL guard page, not the physical seam.

## Fault reporting path

- POSIX handler stores raw `si_addr` → `last_fault_address_` and host RIP →
  `last_fault_rip_` (`exception_handler_posix.cc:165,168,327-332`).
- The harness maps RIP→guest PC via the code cache (`crash_guest=`), but
  prints `last_fault` untranslated (`emulator_headless.cc:1036-1051`).
- A fault at/above `physical_membase_` is deliberately unhandled by both
  `MMIOHandler::ExceptionCallback` (`mmio_handler.cc:398-451`) and
  `Memory::AccessViolationCallback` (`memory.cc:440-466`). A fault **below**
  `physical_membase_` but inside the guard-protected first 64 KiB is likewise
  a genuine crash (not MMIO, not a watched page).

## Special cases (for completeness)

- Guest VA→file-offset views (`memory.cc:216-294`): `0x7F000000-0x7FFFFFFF`
  and `0xA0000000-0xDFFFFFFF` alias physical @ target `0x100000000`;
  `0xE0000000-0xFFFFFFFF` @ `0x100001000` (the +4 KiB quirk).
- The 0xE0000000 +0x1000 host-offset shim is **inert on Linux**
  (`allocation_granularity()` = 4096, `memory_posix.cc:66`;
  gate at `memory.cc:1387-1393`); the JIT's conditional-add sequence
  (`x64_seq_memory.cc:52-59,89-96`) compiles but never fires.

## Verification hook for agents

To confirm `virtual_membase_ == 0x100000000` on a live run: log
`Memory::virtual_membase_` at init (or check an existing boot log for the
mapping-base print if one exists). Any fix work MUST first confirm this, since
the entire NULL-deref reinterpretation rests on it.
