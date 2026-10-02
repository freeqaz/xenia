# 00 — Xenia Source Map for the JIT-Fault Investigation

Where everything lives. Paths relative to the xenia repo root. See
[INDEX.md](INDEX.md) for the investigation overview.

## 1. PPC→x64 JIT backend (where host addresses are computed)

| Path | Role |
|---|---|
| `src/xenia/cpu/backend/x64/x64_emitter.{h,cc}` | x64 instruction emitter; `GetMembaseReg()` |
| `src/xenia/cpu/backend/x64/x64_seq_memory.cc` | **Load/store sequence emission — the guest-EA→host-address computation lives here** |
| `src/xenia/cpu/backend/x64/x64_sequences.cc` | General sequences (arithmetic, shifts, extensions) |
| `src/xenia/cpu/backend/x64/x64_seq_control.cc` | Control-flow sequences |
| `src/xenia/cpu/backend/x64/x64_seq_vector.cc` | Altivec sequences |
| `src/xenia/cpu/backend/x64/x64_backend.cc` | Backend main, code cache |
| `src/xenia/cpu/compiler/passes/register_allocation_pass.cc` | Register allocation |

## 2. PPC frontend / HIR (where PPC semantics are modeled)

| Path | Role |
|---|---|
| `src/xenia/cpu/ppc/ppc_emit_memory.cc` | **PPC load/store decoders (lwz/lwzx/lwzu/stw…) — where EA truncation semantics are chosen** |
| `src/xenia/cpu/ppc/ppc_emit_alu.cc` | add/addi/addis etc. (address arithmetic) |
| `src/xenia/cpu/ppc/ppc_hir_builder.{h,cc}` | PPC→HIR conversion |
| `src/xenia/cpu/hir/opcodes.h` | HIR opcodes (OPCODE_LOAD/STORE/LOAD_MMIO) |
| `src/xenia/cpu/hir/value.cc` | HIR value/type system (I32 vs I64 tracking) |
| `src/xenia/cpu/ppc/ppc_translator.cc`, `ppc_scanner.cc` | Translation pipeline / CFG |

## 3. Guest memory model

| Path | Role |
|---|---|
| `src/xenia/memory.{h,cc}` | membase_/physical_membase_/virtual_membase_; maps guest 4 GiB into host space; heaps |
| `src/xenia/base/mapped_memory_posix.cc` | Linux mmap mapping (host placement on POSIX) |
| `src/xenia/base/memory.h`, `base/mapped_memory.h` | Platform memory primitives |

## 4. Exception / crash reporting

| Path | Role |
|---|---|
| `src/xenia/base/exception_handler_posix.cc` | SIGSEGV capture; `last_fault_address_`, `last_fault_rip_` |
| `src/xenia/base/exception_handler.h` | `GetLastFaultAddress()`, AccessViolationOperation |
| `src/xenia/app/emulator_headless.cc` | Prints `crash_guest=… last_fault=…` (our fork's headless status line) |
| `src/xenia/emulator.cc` | Fault dispatcher (host_fault/guest_fault logging) |

## 5. MMIO / page-watch interception

| Path | Role |
|---|---|
| `src/xenia/cpu/mmio_handler.{h,cc}` | MMIO ranges; `AccessViolationCallback`; decides whether a fault is "handled" MMIO or a real crash |
| `src/xenia/cpu/processor.h` | Links JIT ↔ memory ↔ fault handling |

## 6. Tests

| Path | Role |
|---|---|
| `src/xenia/cpu/ppc/testing/` | PPC assembly semantics tests (`.s` files + `ppc_testing_main.cc`) |
| `src/xenia/cpu/testing/` | HIR/backend instruction tests (`sandbox_main.cc`, per-op tests) |
| `src/xenia/base/testing/memory_test.cc` | Mapping tests incl. 0x100000000 placement |

## Working hypothesis pointer

The fault signature (host fault at membase + exactly 2^32) means a guest EA
reached the host add as a 64-bit value ≥ 2^32 — i.e. somewhere between
`ppc_emit_memory.cc` (EA modeling) and `x64_seq_memory.cc` (host address
computation), a required 32-bit truncation/zero-extension is missing or
skipped. See [02-address-translation.md](02-address-translation.md) for the
mechanism and [03-guest-code-analysis.md](03-guest-code-analysis.md) for which
guest instruction triggers it.
