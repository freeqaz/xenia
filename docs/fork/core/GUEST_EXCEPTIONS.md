# Guest SEH / C++ exception dispatch

Status 2026-10-02 (core lane D2): **pieces 1-4 built for the MSVC C++ catch
path; SEH `__except` and cross-frame RtlUnwind transfers are not.** This is
gap G13 / cluster K10 / lane L9 in `docs/fork/cleanup/DC3_HACK_GAP_ANALYSIS.md`.
The design below was written first (Lane D); "What is built" records how the
implementation follows it and where it deliberately took a simpler route.

## What is built (core-d2)

| piece | where | tested by |
|---|---|---|
| 1 `.pdata` lookup | `cpu/ppc/ppc_unwind.cc` `LookupFunctionEntry` | `xenia-cpu-ppc-tests` `guest_unwind` |
| 2 PPC virtual unwind | `cpu/ppc/ppc_unwind.cc` `VirtualUnwind` | same, 6 prologue shapes; sabotage (no register restore) fails 4/7 |
| 3 dispatch | `kernel/xboxkrnl/xboxkrnl_guest_exceptions.cc` `DispatchGuestException`, called by `RtlRaiseException` | DC3 S2 with `--guest_exception_dispatch_first` (see below) |
| 4 unwind + transfer | same file `UnwindGuestFrames` (`RtlUnwind`), `cpu/backend/x64/x64_guest_unwind.cc` | not exercised end to end yet (no reachable guest catch) |
| `RtlCaptureContext` | same file `CaptureGuestContext`: the real 0xA40-byte CONTEXT | via piece 4 |

How it differs from the design:

- **Virtual unwind executes the prologue symbolically** (stores relative to
  the entry r1, so `subi r12,r1,X; bl __savefpr_N` and `subi r31,r1,X`
  frame pointers work) and recognises the CRT save helpers by
  interpreting their straight-line store bodies, so no symbols or
  `FindSaveRest` lookups are needed. Epilogues are not modelled (never a
  call site).
- **RtlUnwind's transfer is a plain return.** MSVC's RISC CRT calls
  `RtlUnwind(pRN, ReturnPoint, ...)` from `_UnwindNestedFrames` with
  `TargetIp` = the instruction after that call, and the catching frame's
  `__CxxFrameHandler`, on `EXCEPTION_TARGET_UNWIND`, copies the CONTEXT it
  is given (the catching frame's registers) into the CRT's per-thread
  continuation context and replaces it with the context
  `_UnwindNestedFrames` captured. So "restore the context at TargetIp" is
  "return from the export". The walk crosses the `Processor::Execute`
  boundary of the handler call back into the throw chain, as NT continues
  through its dispatcher's frame. Any other RtlUnwind (SEH `__except`,
  `longjmp`, an exit unwind) is logged and returns, as the stub did.
- **The jump to the catch is guest code.** `_JumpToContinuation` loads the
  catching frame's registers and `blr`s into it; the JIT runs that as a
  tail call to a fresh function at the continuation, on top of the throw's
  host frames. Guest state is all in `PPCContext`, so that is correct.
  Instead of jumping into the catching function's host frame at the
  continuation's machine-code address (the design's hard part), the backend
  folds the stale host frames away **when the catching function returns**:
  `Backend::ArmGuestUnwindReturn` matches the throw chain's JIT frames to
  its guest frames one to one (each `[rsp+GUEST_RET_ADDR]` must equal the
  guest unwinder's return address, frame sizes are now kept per
  `X64Function`) and records the host slot of the catching frame's original
  return address. A mismatched guest return then checks a global count (one
  compare when nothing is armed) and, on an exact match of guest target and
  guest r1, does `rsp = slot; ret`. Until then the stale frames stay on the
  host stack, dormant -- the same frames the DTA throw hook longjmps over.
  A frame that is itself an earlier catch continuation is spliced through
  its own record, so repeated catches in one function do not accumulate.
- **Hook order**: the DC3 DTA throw hook still runs first by default.
  `--guest_exception_dispatch_first` (default false) dispatches first and
  calls the hook only for exceptions no guest handler took.

Not built / known limits:

- `__C_specific_handler` (SEH `__try/__except`) is still the "continue
  search" stub, and cross-frame RtlUnwind transfers are not implemented.
- A throw from inside a catch funclet (`throw;`) is dispatched without the
  outer throw's frames: the dispatch record is retired when its RtlUnwind
  arms the host return.
- The CONTEXT handed to handlers carries the live CR/XER/CTR/VMX, not the
  unwound frame's (`_JumpToContinuation` restores CR from it; MSVC rarely
  keeps a value in a non-volatile CR field across a call).
- Each caught exception leaks the dispatcher's small heap vector (its C++
  frame is discarded with the throw's host frames).

## Before core-d2 ("Today" as Lane D wrote it)

- `RtlRaiseException` (`kernel/xboxkrnl/xboxkrnl_debug.cc`) handles two codes:
  - `0x406D1388`, SetThreadName;
  - `0xE06D7363`, a C++ throw. It calls `g_cpp_throw_hook` if one is set, then
    `xe::debugging::Break()`.

  Every other code also breaks. Nothing dispatches, so `MILO_TRY`/`MILO_CATCH`
  and every `__try`/`__except` in a title are dead code.
- `RtlUnwind` is a logged no-op, and `__C_specific_handler` always answers
  "continue search". `RtlCaptureContext` memsets a guessed 0x200 bytes
  (`xboxkrnl_rtl.cc`).
- The DC3 DTA channel recovers from a throw that its own evaluation raised. Its
  host code arms a `setjmp` and calls `processor->Execute`. The throw hook
  (`xboxkrnl_cpp_throw_hook.h`) then `longjmp`s back over the JIT frames and the
  export-shim frames. This proves something the design below depends on: on
  this backend, discarding host frames between a guest throw and an outer host
  frame is safe. JIT frames hold no RAII state, and guest state lives in
  `PPCContext`.

## What a real implementation needs

The guest CRT on Xbox 360 is statically linked: `_CxxThrowException`,
`__CxxFrameHandler`, the catch funclets and `__C_specific_handler` are guest
code, and only `RtlRaiseException` / `RtlUnwind` (and helpers) are kernel
exports. So the kernel side needs four pieces, in this order.

1. **Function table lookup** (`RtlLookupFunctionEntry`). Xbox 360 `.pdata`
   holds the 8-byte compact entries
   `{BeginAddress, PrologLength:8 | FunctionLength:22 | ThirtyTwoBit:1 | ExceptionFlag:1}`.
   When `ExceptionFlag` is set, the language handler and its data are the two
   words just before `BeginAddress`. The section is reachable today through
   `XexModule::GetPESection(".pdata")`, and the entries are sorted, so a binary
   search works.
2. **Virtual unwind** (`RtlVirtualUnwind`, PPC). Decode the prologue up to the
   PC to recover the caller's context:
   - `mflr r12`;
   - `bl __savegprlr_N` / `__savefpr_N` / `__savevmx_N` (the save helpers,
     which `XexModule::FindSaveRest` already locates);
   - `stw r12,-8(r1)`;
   - `stwu r1,-X(r1)` / `stwux`.

   Then:
   - the caller's `r1` is the back chain `*(r1)`;
   - the caller's `LR` is `*(r1_caller - 8)`;
   - the non-volatiles come back from the save area.

   This piece also gives real guest stack traces (`RtlCaptureStackBackTrace`,
   crash reports), so it is useful on its own and should land first, with
   unit tests over hand-assembled prologues in `xenia-cpu-ppc-tests`.
3. **Dispatch** (`RtlRaiseException` → `RtlDispatchException`). Capture the
   context, then walk the frames with (1) and (2). For each frame with
   `ExceptionFlag` set, call the guest handler
   `handler(ExceptionRecord, EstablisherFrame, ContextRecord, DispatcherContext)`
   through `processor->Execute` on the same thread. A handler that finds a catch
   does not return: it calls `RtlUnwind`.
4. **Unwind and transfer** (`RtlUnwind(TargetFrame, TargetIp, Record, ReturnValue)`).
   Walk again from the current frame to `TargetFrame`, calling each handler with
   `EXCEPTION_UNWINDING` (this runs the destructors). Then restore the target
   frame's non-volatiles and `r1`, set `r3 = ReturnValue`, and resume guest
   execution at `TargetIp` inside the target function.

   This last step is the hard part. The JIT runs guest calls on the host stack,
   so "resume at `TargetIp` in frame F" means:

   - **Find F's host frame.** Walk the host stack from the `RtlUnwind` export's
     caller outward. A JIT frame is identified by its host return address, which
     must lie in the code cache. Its size is the function's emitter
     `stack_size()`, which is per function and not recorded today, so
     `X64Function` must keep it. Its guest return address is at
     `[rsp + StackLayout::GUEST_RET_ADDR]`. Guest frames and JIT frames
     correspond one to one: MSVC tail calls run after the epilogue, and the JIT
     pops the frame on a tail call too. So the k-th guest frame from the throw is
     the k-th JIT frame, after skipping thunk and export frames, whose return
     addresses lie outside the code cache.
   - **Find the host PC of `TargetIp`:**
     `GuestFunction::MapGuestAddressToMachineCode(TargetIp)`. The catch
     continuation is a branch target of the try block's normal exit, so it is a
     block start. At a block start the x64 backend holds no guest value in a
     host register (allocation is block-local); only `rsi` (context) and `rdi`
     (membase) are live. Assert that the source map has an exact entry for
     `TargetIp`. If it does not, the function needs a recompile with a label
     there.
   - **Jump.** A small emitted trampoline sets `rsp` to F's frame, `rsi`/`rdi`,
     and `jmp`s to the host PC. Like the DTA hook's `longjmp`, this discards the
     host frames between the two points: the export shims, `RtlUnwind`, the
     `Execute` of the handler, and the throwing chain. The export call path holds
     no lock across the guest call; keep it that way and assert it in Checked
     builds.

`__C_specific_handler` (SEH `__try/__except`) is a kernel export on this
platform. It becomes implementable once (1) to (4) exist: evaluate the scope
table's filters through `Execute`, then call `RtlUnwind`.

## Interaction with the DTA channel hook

Keep `g_cpp_throw_hook` working throughout:

- During bring-up, call the hook first, exactly as now. A channel evaluation
  that armed a recovery point still `longjmp`s out, whether or not the guest
  has a catch.
- After step 4 works, change the order: dispatch first, and call the hook only
  when the dispatch finds no handler (an unhandled exception).
- The S2 criterion `{no_such_func 1}` → `=> !! refused: script error` is then
  met by the real `MILO_CATCH`. The hook can be deleted only after S2 passes
  without it, which is lane L9's definition of done.

## Tests

- **Unit**, in `xenia-cpu-ppc-tests` built-ins, like `guest_function_override`:
  - (1) lookup on a hand-written `.pdata`;
  - (2) virtual unwind of the four prologue shapes.
- **End to end:** a hand-assembled guest
  `try { throw } catch { r3 = 42 }`. It needs a minimal `__CxxFrameHandler`;
  the real one is in every title's CRT, so a recorded fragment from DC3's
  `debug.xex` could serve as the fixture.
- **Harness:** S2 with the hook removed.

## Size

L to XL, as the gap analysis says. Pieces (1) and (2) are about a week and
upstreamable alone. Piece (4) is the risk: host-frame discovery needs per-function
frame sizes, and every JIT invariant it relies on above must be asserted rather
than assumed.
