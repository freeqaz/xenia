# Guest SEH / C++ exception dispatch: design

Status 2026-10-02 (Lane D): **designed, not implemented.** This is gap G13 /
cluster K10 / lane L9 in `docs/fork/cleanup/DC3_HACK_GAP_ANALYSIS.md`. It is a
real upstream gap, and once built it is worth upstreaming.

## Today

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
