# DC3 Boot Debugging Tips & Strategies

*Compiled from sessions 10-13 (2026-02-22 to 2026-02-23)*

This document captures hard-won debugging knowledge from bringing up the Dance Central 3
decompilation XEX in Xenia on Linux (headless mode). Many of these lessons apply generally
to Xbox 360 emulator debugging and XEX reverse engineering.

---

## 1. Xenia JIT Architecture (x64 backend)

### Memory Map (host addresses)

| Range | Purpose |
|-------|---------|
| `0x40000000-0x7FFFFFFF` | **UNMAPPED** — XAM.xex would live here on real hardware |
| `0x80000000-0x9FFFFFFF` | **JIT Indirection Table** — 512MB, maps guest address → JIT code pointer |
| `0xA0000000-0xAFFFFFFF` | **Generated Code** — JIT-compiled x86-64 code lives here |
| `mapping_base_` | **Virtual membase** (rdi in JIT) — guest 0x00000000 maps here |
| `mapping_base_ + 0x100000000` | **Physical membase** — NOT at 0x80000000! |

### Indirection Table

The indirection table is a 512MB region at host `0x80000000`. For a guest function at
address G (where `0x80000000 <= G < 0xA0000000`), the 4-byte JIT code pointer is stored
at host address G itself. The JIT reads it with:

```x86
mov eax, dword [ebx]    ; ebx = guest address, eax = JIT code pointer
call rax                 ; call the compiled function
```

**Key constant**: `indirection_default_value_` = address of `resolve_function_thunk`.
When a slot still has this value, calling it triggers lazy compilation.

### Danger: Out-of-Range Indirection Table Access

If guest code does `bctrl` with CTR pointing outside `[0x80000000, 0x9FFFFFFF]`
(e.g., XAM COM vtable at `0x40002830`), the JIT reads from unmapped host memory → SIGSEGV.

**Fix**: Added bounds check in `X64Emitter::CallIndirect`:
```
mov eax, ebx
sub eax, 0x80000000
cmp eax, 0x1FFFFFFF
jb  in_range        ; fast path: read indirection table
mov eax, <resolve_thunk_addr>  ; slow path: use resolve thunk
jmp resolved
in_range:
mov eax, dword [ebx]
resolved:
```

### Danger: Calling ResolveFunction as C from JIT Context

The JIT keeps guest PPC state in host registers:
- `rsi` = PPC context pointer
- `rdi` = memory base pointer
- `r10`, `r11` = guest GPR mappings
- `xmm4-xmm15` = guest vector register mappings

All of these are **volatile** in the SysV x86-64 ABI. Calling a C function (like
`ResolveFunction`) directly from JIT code clobbers them all. The **resolve function thunk**
(`EmitResolveFunctionThunk` in x64_backend.cc) properly saves/restores volatile regs.

**Rule**: Never call C functions directly from generated JIT code. Always go through a
thunk that saves/restores volatile registers, or use the resolve function thunk for
function resolution.

**Bug found**: The "old-style resolve" else-path in `CallIndirect` has a register-ordering
bug on Linux: it sets `esi = target` before `rdi = rsi(context)`, clobbering the context.
Correct order (matching the resolve thunk): `mov rdi, rsi` first, then `mov esi, target`.

### Extern Function Compilation

Kernel imports (xboxkrnl.exe exports) are marked `Behavior::kExtern` and have guest code
`sc 2; blr` at their thunk address. The PPC frontend compiles `sc 2` (LEV=2) as
`CallExtern(function)` which calls the C++ handler.

**Original bug**: `DemandFunction` skipped compilation for extern functions → `machine_code_`
stayed null → indirect calls via the indirection table got 0 → `jmp(0)` → SIGSEGV at RIP=0.

**Fix** (processor.cc `DemandFunction`): Compile extern functions' guest code so the
indirection table has valid JIT code. Direct calls still use the fast `CallExtern` path.

---

## 2. Guest Memory Debugging

### Null Pointer with Negative Offset

Guest code `lwz rX, -4(r11)` where `r11 = 0` computes:
- host EA = `virtual_membase + 0 + (-4)` = `virtual_membase - 4`
- This wraps to an address just below virtual_membase

The MMIO/SIGSEGV handler only catches faults within `[virtual_membase, memory_end]`.
Faults below virtual_membase are not handled → crash.

**Fix**: Map a 64KB guard page below virtual_membase:
```cpp
mmap(vmbase - 0x10000, 0x10000, PROT_READ,
     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
```

### Interpreting Fault Addresses

When you see a crash dump like:
```
Fault address: host=0x0000000000010117 guest=0x00010117 (READ)
```

The guest address is computed as `host - virtual_membase`. If the host address is very
small (like `0x10117`), it's NOT a guest memory access — it's a raw host pointer
dereference, likely from corrupted JIT state (e.g., clobbered membase register).

### Xbox 360 Heap Debug Patterns

| Value | Meaning |
|-------|---------|
| `0xFEEEFEEE` | MSVC debug fill for freed memory |
| `0xCDCDCDCD` | MSVC debug fill for uninitialized heap |
| `0xBEBEBEBE` | Xenia sentinel for uninitialized stack/LR |
| `0xFEEDF00D` | Xenia indirection table default (resolve thunk) |
| `0x536F7264` | Appears as "Sord" in ASCII — corrupted heap free-list metadata |

If you see `r30=0x536F7264, r25=0xFEEEFEEE` in a heap function crash, this is a
**use-after-free** or **heap corruption** pattern, NOT a string reference.

---

## 3. KernelModule / XAM Debugging

### Trampoline Location

KernelModule (HLE) trampolines are allocated at guest `0x80040000-0x801C0000`:
```cpp
heap->AllocRange(0x80040000, 0x801C0000, kTrampolineSize, ...)
```

Each trampoline is 8 bytes: `sc 2; blr` (syscall-and-return).

### XAM COM Vtable Problem

On real Xbox 360, `xam.xex` loads at ~`0x40000000` with real code. In Xenia, XAM is
HLE'd as a KernelModule — no code exists at `0x40000000+`.

If guest code has a COM-style vtable pointing to XAM functions (e.g., `0x40002830`),
calling through the vtable hits unmapped memory. The NUI/Kinect subsystem is the main
source of these vtable pointers in DC3.

**Symptom**: SIGSEGV at low host addresses during `bctrl` execution.
**Fix**: Bounds check in CallIndirect + no-op return stub for unresolvable functions.

### "No function found" Addresses to Watch

| Address | Meaning |
|---------|---------|
| `0x40002830` | XAM COM vtable entry — NUI interface function |
| `0x38600000` | PPC instruction `li r3, 0` — garbage in callback list |
| `0x00000000` | Null function pointer — uninitialized callback |

These are logged as `ResolveFunction(XXXXXXXX): no function found — using no-op stub`
and indicate the game is trying to call through uninitialized or HLE'd interfaces.

---

## 4. CRT Static Initialization Debugging

### CRT Table Layout

The DC3 XEX has two CRT tables in `.data`:
- `__xc_a..__xc_z`: C++ static constructors (~390 entries)
- `__xi_a..__xi_z`: C initializers (~3 entries)

Each entry is a 4-byte function pointer. The CRT runtime iterates these and calls each
non-null entry before `main()`.

### CRT Sanitizer Strategy

Many entries point to `0x82000000` (the PE header base) due to unresolved symbols that
the linker's `/FORCE` option resolved to the image base. These must be nullified before
execution.

**Bisect methodology** for identifying which constructors cause problems:
1. Start with all constructors enabled
2. Use `--dc3_crt_skip_indices=X-Y` to skip ranges
3. Binary search to find the specific culprit(s)
4. Log output: `CRT table __xc_a..__xc_z: N total, M valid, K nullified-skip`

### NUI Constructors (98-330)

~230 CRT entries in the 98-330 range are NUI SDK constructors that call into unresolved
NUI internal functions. These MUST be skipped — they cause heap corruption when they
try to use the NUI subsystem.

---

## 5. Build System Tips

### Clang ICE (Internal Compiler Error)

Building with high parallelism (`-j$(nproc)`) can cause clang to crash with SIGBUS on
large files like `emulator.cc`. Use `-j4` or `-j2` instead:
```bash
make -C build/ xenia-headless -j4
```

### Incremental Rebuilds

For fastest iteration, rebuild only the changed target:
```bash
# After changing x64_emitter.cc:
make -C build/ xenia-cpu-backend-x64 -j2
# Then relink:
make -C build/ xenia-headless -j2
```

### Test Command

```bash
timeout 30 ./build/bin/Linux/Checked/xenia-headless \
  --target=/path/to/dc3-decomp/build/373307D9/default.xex \
  2>/tmp/dc3_stderr.log
echo "exit: $?"
```

Exit codes:
- `0`: Clean exit
- `124`: Timeout (the game ran without crashing!)
- `134`: SIGABRT (assertion failure)
- `139`: SIGSEGV (crash)

### Stderr vs Stdout

- **stdout**: Xenia log output (XELOGI, XELOGE, etc.)
- **stderr**: Periodic thread status reports from the headless runner + assertion messages

The thread status report (every 3 seconds) shows:
- Thread LR, SP, key registers, CTR
- Whether the host RIP is in JIT code or host/runtime code
- Import thunk health checks
- Heap state
- Guest code disassembly near LR
- Stack backtrace

---

## 6. Common Crash Patterns & Diagnosis

### Pattern: SIGSEGV at RIP=0x0
**Cause**: Indirection table returned 0 (null machine_code for an extern function).
**Fix**: Compile extern functions in `DemandFunction` (processor.cc).

### Pattern: SIGSEGV at very low host address (< 0x80000000)
**Cause**: JIT `CallIndirect` tried to read the indirection table for a guest address
outside the `[0x80000000, 0x9FFFFFFF]` range.
**Fix**: Bounds check in `CallIndirect` with resolve thunk fallback.

### Pattern: SIGSEGV at host address just below virtual_membase
**Cause**: Guest null pointer + negative offset (e.g., `lwz rX, -4(0)`).
**Fix**: Guard page below virtual_membase.

### Pattern: Assertion `(target_address) != 0` in ResolveFunction
**Cause**: Guest code called a null function pointer (`bctrl` with `CTR=0`).
**Fix**: Handle null target gracefully in ResolveFunction, return no-op stub.

### Pattern: Game stuck in tight loop calling garbage addresses
**Cause**: A callback notification list has uninitialized function pointers.
The no-op stub returns but the loop continues iterating forever.
**Diagnosis**: Check the thread status report — look at LR, CTR, and the
disassembled guest code near LR. Look for `lwz rX, offset(rY); mtctr rX; bctrl`
patterns in a loop.
**Fix**: Identify the callback list and either empty it or stub the dispatch function.

### Pattern: "Sord" / 0xFEEEFEEE in heap function crash
**Cause**: Heap corruption from CRT constructors calling unresolved NUI functions.
**Fix**: Skip NUI CRT constructors (indices 98-330).

---

## 7. Diagnostic Instrumentation

### Indirection Table Health Check
```cpp
uint32_t check_addrs[] = {0x8395C668, 0x8395C000, 0x8395B000, 0x822E0000};
for (auto addr : check_addrs) {
    uint32_t* slot = reinterpret_cast<uint32_t*>(0x80000000 + (addr - 0x80000000));
    XELOGI("Indirection table [{:08X}] = {:08X}", addr, *slot);
}
```
Expected value `0xA0000170` = resolve thunk. Any other value = compiled function address.

### Import Thunk Verification
```cpp
auto* mem = memory->TranslateVirtual(thunk_addr);
uint32_t word0 = xe::load_and_swap<uint32_t>(mem + 0);
// Expected: 0x44000042 (sc 2) for kernel imports
```
If thunk memory is all zeros, the import was not properly resolved.

---

## 8. Key Source File Reference

| File | Purpose |
|------|---------|
| `src/xenia/emulator.cc` | DC3 patches (NUI stubs, CRT sanitizer, guard pages) |
| `src/xenia/cpu/processor.cc` | Function resolution, DemandFunction, extern compilation |
| `src/xenia/cpu/backend/x64/x64_emitter.cc` | JIT code generation, CallIndirect, ResolveFunction |
| `src/xenia/cpu/backend/x64/x64_code_cache.cc` | Indirection table, PlaceGuestCode |
| `src/xenia/cpu/backend/x64/x64_backend.cc` | Resolve thunk, exception handler |
| `src/xenia/kernel/kernel_module.cc` | KernelModule trampoline allocation |
| `src/xenia/cpu/xex_module.cc` | XEX loading, CommitExecutableRange, import resolution |
| `src/xenia/base/exception_handler_posix.cc` | SIGSEGV handler |
| `src/xenia/base/memory_posix.cc` | AllocFixed (mprotect/mmap) |
| `src/xenia/app/emulator_headless.cc` | Headless runner, thread status reports |
