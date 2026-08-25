# Fork cleanup review

**Date:** 2026-08-25
**Reviewed:** `ffd4828bce2dd4e39f0da84e948bb2045be7e069` (branch `frag-alloc-trace` at time of review)
**Merge-base with upstream:** `c7f61342d7061b8264e4b988b8d2e03351b6e088` (`upstream/master`, 2026-01-20, "[GPU/D3D12] Convert gamma-as-linear red to gamma in transfers to stencil")
**Commits since fork point:** 177

```
183 files changed, 47520 insertions(+), 446 deletions(-)
  src/   128 files, +28508 / -433
  docs/   38 files, +13836 / -0     (session logs / wiki -- explicitly out of scope)
  tools/  10 files, +4173           (dc3_* scripts)
  root     5 files, +842            (CLAUDE.md, analyze_poolalloc.py, test_headless.sh, .gitignore, xenia-build)
```

**Line-number caveat.** All references were verified against a clean tree at the reviewed
sha. `emulator.cc` references were verified one commit later, at `11c32a395`
("emulator: --si_hook_vas"), which added 41 lines. The working tree has since moved to
branch `rb3-si-refresh` (`c8975caae`), which adds a further ~93 lines to `emulator.cc`.
Line numbers in `emulator.cc` may therefore have drifted by up to ~130 lines; every other
file is exact. Symbol names in each item are unique enough to `grep` for if a number is off.

**Scope note.** Per the review brief, `docs/` content (including `docs/jit-fault-wiki/`),
`third_party/` submodule pointer bumps, and the overall cvar-gated DC3/RB3 architecture are
not treated as slop. Checked-config assert demotions that mirror Release semantics *and* log
a warning are also not flagged; demotions that are silent, or that mutate state rather than
merely continuing, are.

**The headline.** The fork's *title-specific* work is, with a handful of exceptions,
correctly gated behind cvars and `title_id_` checks -- that part was done well. The risk is
concentrated somewhere else: a set of **ungated "make the guest stop crashing" changes in the
shared PPC frontend, x64 backend, MMIO fault handler, kernel IO/RTL layer, and Vulkan command
processor.** Those change behaviour for *every* title, they mostly convert loud failures into
silent ones, and several of them would make Xenia unable to diagnose a bug in any other game.
That is the cleanup that matters; the DC3 code volume in `emulator.cc` is a distant second.

---

## 1. Critical

Correctness risk for unrelated titles, or host-stability risk. Ordered by severity.

### C1. Unimplemented PPC instructions are now a silent no-op for every title

`src/xenia/cpu/ppc/ppc_emit-private.h:25`

```c
#define XEINSTRNOTIMPLEMENTED() do {} while(false)
```

Upstream logged `XELOGE("Unimplemented instruction: {}", __func__)` *and* asserted. The fork
deleted both. Every unimplemented PPC instruction in every game now translates to nothing at
all, with zero diagnostics -- the guest silently computes wrong results and the log is clean.
This is the single most damaging change in the fork: it removes Xenia's ability to tell anyone,
ever, that a title hit an unimplemented opcode.

Compounding it, `src/xenia/cpu/ppc/ppc_hir_builder.cc:177` deleted
`XELOGE("Invalid instruction {:08X} {:08X}", ...)` outright, and `ppc_hir_builder.cc:197-203`
moved the "Unimplemented instr ... report the game to Xenia developers" `XELOGE` *inside* the
`if (cvars::break_on_unimplemented_instructions)` branch, so the message only appears when
you have already opted into breaking.

**Fix:** restore the `XELOGE` in the macro unconditionally; keep the assert behind
`break_on_unimplemented_instructions` (it already is, via `DebugBreak()`). Move the
`ppc_hir_builder.cc` log back outside the cvar branch, rate-limited if the volume was the
original problem.

### C2. Unmapped guest reads silently return zero for every title

`src/xenia/cpu/mmio_handler.cc:542-560`

```c
// Soft fault: for reads from unmapped guest memory (e.g. stack guard
// pages), zero the destination register and skip the faulting instruction
// instead of crashing.  This keeps decomp/stub guests alive.
if (!is_write) {
  ...
  ex.ModifyIntRegister(decoded_load_store.value_reg) = 0;
  ex.set_resume_pc(rip + decoded_load_store.length);
  return true;
}
```

Ungated, no cvar, no title check. Any read access violation in any game is now swallowed:
the destination register is zeroed and execution resumes. Real memory-corruption bugs,
uninitialised-pointer dereferences, and stack overruns all become "the game behaved oddly"
instead of a crash with a PC. Combined with C1, a title can be quietly wrong from two
independent directions with an empty log.

**Fix:** gate on a cvar (`--soft_fault_unmapped_reads`, default false) and log the first N
occurrences at `XELOGW` with the guest EA and PC even when enabled.

### C3. DC3-specific guest address range hardcoded in the shared MMIO fault handler

`src/xenia/cpu/mmio_handler.cc:485-501`

```c
// XEX image DATA range: host base + 0x83320000 to host base + 0x836C0000
uint64_t data_start = reinterpret_cast<uint64_t>(virtual_membase_) + 0x83320000;
uint64_t data_end   = reinterpret_cast<uint64_t>(virtual_membase_) + 0x836C0000;
if (fault_addr >= data_start && fault_addr < data_end) {
  memory::Protect(page_base, memory::page_size(), memory::PageAccess::kReadWrite, nullptr);
  XELOGW("DC3: Re-enabled write at host {:016X} ...");
  return true;
}
```

Two magic constants from one build of one game, applied to every title, in the access-violation
path. For any other game whose `.data` happens to land in `[0x83320000, 0x836C0000)`, a genuine
read-only-page write fault is silently converted into "make the page writable and continue",
defeating write-watch and NtProtect semantics. The constants are also stale by construction --
they came from a DC3 build whose PE layout the fork's own notes say changes on every relink.

**Fix:** gate on `title_id == 0x373307D9` at minimum; better, derive the range from the loaded
module's PE section table instead of hardcoding, and only apply it under a `--dc3_*` cvar.

Also in this file: `mmio_handler.cc:25` `DECLARE_bool(rb3dx_alloc_probe)` creates a layering
inversion -- `src/xenia/cpu/` now depends on a cvar defined up in `src/xenia/emulator.cc:221`.
Same at `src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc:27`.

### C4. Null and unresolvable guest calls become silent no-ops for every title

`src/xenia/cpu/backend/x64/x64_seq_control.cc:187, 200, 212, 224, 236, 248, 260`

```c
if (!i.src1.value) return;   // CALL
if (!i.src2.value) return;   // CALL_TRUE_I8 .. CALL_TRUE_F64
```

`src/xenia/cpu/backend/x64/x64_seq_control.cc:279, 301, 318, 335, 352, 369, 386`

```c
if (i.src1.is_constant) { auto constant = i.src1.constant(); if (constant == 0) return; ... }
```

`src/xenia/cpu/backend/x64/x64_emitter.cc:737-757, 816-819`

```c
// Fast path for null function pointers: skip the call entirely.
test(ebx, ebx);
if (instr->flags & hir::CALL_TAIL) je(epilog_label(), ...); else jz(skip_null_call, ...);
```

`src/xenia/cpu/backend/x64/x64_emitter.cc:567-576, 588-601, 620-634, 636-647`

```c
static uint64_t GetNoopReturnStub(ThreadState* thread_state);  // a single 0xC3
...
XELOGE("ResolveFunction(00000000): null target -- using no-op stub (count={})", n + 1);
return GetNoopReturnStub(thread_state);
```

Upstream had `assert_not_zero(target_address)` and `assert_not_null(fn)` here. Calling through
a null function pointer is now a no-op that returns an undefined value, in every game. This is
the correct behaviour for a decomp target with `/FORCE`-linked unresolved externs; it is exactly
wrong for a retail title, where a null vtable slot means the object graph is corrupt and the
emulator should say so. The rate-limited `XELOGE` helps, but the comment at `x64_emitter.cc:749`
("78M+ calls from uninitialized objects") shows the volume makes the log useless in practice.

**Fix:** put the whole family behind one cvar (`--tolerate_null_guest_calls`, default false,
documented as a decomp-support flag). With it off, restore the asserts. This is a mechanical
change -- the branches are already isolated.

### C5. Frame-in-flight backpressure removed in the Vulkan command processor

`src/xenia/gpu/vulkan/vulkan_command_processor.cc:3495-3507`

```c
bool headless = !graphics_system_->presenter();
if (headless && await_submission > 0) {
  CheckSubmissionCompletionAndDeviceLoss(0);      // <-- do not actually wait
} else {
  CheckSubmissionCompletionAndDeviceLoss(await_submission);
}
...
if (!headless && completed_submission < await_submission) return false;
```

The `headless` predicate is `!graphics_system_->presenter()`, which is true for *any* run
without a window -- including `xenia --gpu=vulkan` trace dumps and any future automated
capture for an unrelated title. In that mode the CP no longer waits for the submission it was
asked to wait for, so buffers and descriptor pools can be recycled while the GPU is still
reading them. That is a use-after-free on the device side; it will produce corruption or a
device-lost that looks like a driver bug.

**Fix:** gate on an explicit `--headless_skip_submission_wait` cvar rather than inferring from
presenter absence, and only for the DC3/RB3 capture path. Better: fix the underlying deadlock
(the comment at `:3493` says the game thread needs `EVENT_WRITE_SHD`/`WAIT_REG_MEM` serviced)
by servicing those from the wait loop instead of not waiting.

### C6. All non-copy draws dropped whenever there is no presenter

`src/xenia/gpu/vulkan/vulkan_command_processor.cc:2769-2773`

```c
// HEADLESS: Skip non-copy draws unless this is a capture frame or
// force_all_draws is enabled.
if (!graphics_system_->presenter() && !headless_render_frame_ && !cvars::force_all_draws) {
  return true;
}
```

Same over-broad `!presenter()` predicate. Every headless Vulkan run of every title silently
renders nothing, which breaks trace-dump / regression-capture workflows that have nothing to do
with DC3. `--force_all_draws` exists as an escape hatch but defaults off.

**Fix:** invert the default -- render all draws unless a `--headless_capture_only_frames` cvar
is set. The interval-capture optimisation is a DC3 throughput concern, not a correctness one.

### C7. Async file IO always completes synchronously, for every title

`src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:253-259, 320-322, 389-391`

```c
// Note: We always complete synchronously (even for async files),
// so we should NOT return STATUS_PENDING. ...
```

The fork deleted `if (!file->is_synchronous()) result = X_STATUS_PENDING;` from `NtReadFile`,
`NtReadFileScatter`, and `NtWriteFile`, and deleted the entire async-path `else` branch that
set `io_status_block->status = X_STATUS_PENDING`. Every title that opens a file with
`FILE_FLAG_OVERLAPPED` now sees synchronous completion. Titles that rely on `STATUS_PENDING`
to drive their own IO state machine (queue depth accounting, completion-port style dispatch)
will mis-sequence. The XAPILIB `GetOverlappedResult` spin the comment describes is a real bug,
but the fix was applied globally rather than to the file object that exhibited it.

**Fix:** keep the synchronous completion but preserve the `STATUS_PENDING` contract by writing
the completed status into the `OVERLAPPED` the caller passed, or gate the behaviour change on a
cvar until the XAPILIB path is understood.

### C8. Critical sections are silently re-initialised and force-released, for every title

`src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc:539-544`

```c
// Unconditional auto-init: if the CS was never initialized (type != 1),
// initialize it now.  This handles the DC3 decomp's /FORCE-linked unresolved
// extern pointers (e.g. gMemLock) ...
if (cs->header.type != 1) {
  xeRtlInitializeCriticalSection(cs, cs.guest_address());
}
```

`src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc:610-626`

```c
if (cs->owning_thread != XThread::GetCurrentThread()->guest_object()) {
  XELOGW("RtlLeaveCriticalSection: ownership mismatch ... -- forcing release");
  cs->owning_thread = XThread::GetCurrentThread()->guest_object();   // <-- mutates
}
if (cs->recursion_count <= 0) {
  XELOGW("RtlLeaveCriticalSection: recursion_count={} -- clamping to 1", ...);
  cs->recursion_count = 1;                                            // <-- mutates
}
```

The auto-init is completely ungated (the comment says it handles the DC3 decomp's unresolved
externs, but the code has no title check). If a legitimately-held CS in another game has its
memory transiently misread, this silently resets `lock_count` to -1 and drops the lock, causing
data races the title never had on hardware.

The `RtlLeaveCriticalSection` demotions do log, which the brief permits -- but these two go
past "continue where Release would have continued" into *rewriting guest lock state*. Forcing
ownership to the calling thread converts a detectable double-unlock / cross-thread-unlock bug
into a corrupted lock.

**Fix:** title-gate the auto-init (or put it behind `--decomp_autoinit_critical_sections`).
In `RtlLeaveCriticalSection`, log and `return` without mutating -- that *is* the Release
semantic. Related, at `:554-562`, the "always try the fast CAS path once" change is an ungated
behaviour change to every title's lock acquisition; it looks correct and upstreamable, but it
belongs in its own commit with that framing.

### C9. Guest stack walks dereference unvalidated guest pointers from the exception handler

`src/xenia/emulator.cc:3821-3869` (and `:3800-3816`, `:3826-3836`)

```c
for (; frame < 20000 && sp >= 0x70000000 && sp < 0x78000000; frame++) {
  auto* host_ptr = memory_->TranslateVirtual<uint8_t*>(sp);
  if (!host_ptr) break;                        // <-- dead: TranslateVirtual never returns null
  ... load_and_swap<uint32_t>(host_ptr), host_ptr + 4, host_ptr + 8 ...
  bc_ptr = memory_->TranslateVirtual<uint8_t*>(back_chain - 8);
```

`Memory::TranslateVirtual` is `virtual_membase_ + guest_address` (see `src/xenia/memory.h:320`);
it cannot return null, so every `if (!ptr) break` guard in this family is dead code. The walk
therefore reads whatever host memory sits at those addresses. It runs *inside the exception
handler for a guest fault*, so a decommitted page anywhere in `[0x70000000, 0x78000000)` turns a
recoverable, diagnosable crash into a nested host SIGSEGV with no output at all. This affects
every title.

The same walk is up to 20,000 frames deep and its log-sampling predicate
(`frame < 30 || frame % 100 == 0 || best_lr != last_lr`) fires on essentially every frame of a
non-recursive stack, so a healthy deep crash emits thousands of `XELOGE` lines from a fault
handler.

The identical unchecked-walk pattern appears in the kernel too:
`src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:378-390` (`KeDelayExecutionThread`) and
`:585-608` (`xeNtSetEvent`, `rb3_trace_shutdown`) both `TranslateVirtual(sp)` after checking
only `sp > 0x70000000` -- no upper bound before the first dereference. Both are behind cvars,
which is the right pattern; the missing bound is still a host-crash vector when enabled.

**Fix:** route every guest-stack read through `memory_->LookupHeap(addr)->QueryProtect(...)` --
the fork's own RB3 probes already do this at `emulator.cc:1272-1279`, so the helper exists.
Cap the frame count via `--crash_stack_walk_frames` (default 64). Delete the dead null guards
so the next reader doesn't trust them.

### C10. Detached probe threads capture `Memory*` and outlive it

`src/xenia/emulator.cc:4249, 4260, 4269, 4281, 4385`

```c
std::thread([probe_mem]() { Rb3dxUiProbeThread(probe_mem); }).detach();
std::thread([si_mem]()    { Rb3dxSiProbeThread(si_mem);    }).detach();
std::thread([hv_mem]()    { Rb3dxSiHookVerifyThread(hv_mem); }).detach();
std::thread([poke_mem]()  { Rb3dxSkipCalibrationPokeThread(poke_mem); }).detach();
```

Five detached threads with `for (;;)` bodies (`:1299, :1376, :1500, :1586`), no stop flag, no
join, no lifetime tie to the title. `TerminateTitle()` (`:3407`) does not stop them and
`~Emulator()` (`:3253`) destroys `memory_` while they are mid-`LookupHeap`. Any run with
`--rb3dx_ui_probe`, `--si_probe`, `--si_hook_verify`, or `--rb3dx_skip_calibration` has a
guaranteed use-after-free on shutdown. `Rb3dxSkipCalibrationPokeThread` (`:1326-1334`)
additionally writes a guest byte forever with no synchronisation and no exit condition.

A sixth detached thread with the same shape is in the GPU:
`src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc:486` spawns a detached pipeline-serialisation
thread capturing `this`.

**Fix:** own them. `std::vector<std::thread>` on `Emulator` plus an `std::atomic<bool>
probes_should_stop_`, joined in `TerminateTitle()` and the destructor. This is ~20 lines and
removes an entire class of shutdown crash.

### C11. Unconditional recursive delete of the user's DC3 content directory

`src/xenia/dc3_hack_pack.cc:6684-6693`, called from `src/xenia/emulator.cc:4416`

```c
void Dc3MaybeCleanStaleContentCache(const std::filesystem::path& content_root) {
  auto dc3_content = content_root / "373307D9";
  if (std::filesystem::exists(dc3_content)) {
    XELOGI("DC3: Cleaning stale content cache at {}", xe::path_to_utf8(dc3_content));
    std::error_code ec;
    std::filesystem::remove_all(dc3_content, ec);
```

`remove_all` on a directory under the user's content root, on every DC3 launch, with no cvar
and no confirmation. It is title-scoped so it cannot touch another game's saves -- but for DC3
it destroys real save data, and the name says "maybe" and "stale" while the code says
"unconditionally, everything".

**Fix:** gate behind `--dc3_clean_content_cache` (default false), and narrow the target to the
specific cache subdirectory rather than the whole title content tree.

### C12. `AllocFixed` on POSIX no longer allocates -- it mprotects

`src/xenia/base/memory_posix.cc:88-104`

```c
void* AllocFixed(void* base_address, size_t length, AllocationType, PageAccess access) {
  uint32_t prot = ToPosixProtectFlags(access);
  if (mprotect(base_address, length, prot) == 0) return base_address;   // <-- new fast path
  void* result = mmap(base_address, length, prot, MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
```

Upstream unconditionally `mmap(MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS)`, which returns
**zeroed** pages. The fork's fast path `mprotect`s the existing mapping instead, so a guest
allocation now returns whatever the previous tenant left behind. Console `NtAllocateVirtualMemory`
hands out zeroed pages; every title that assumes that now reads stale data. The fallback also
changed `MAP_PRIVATE` to `MAP_SHARED`.

This is almost certainly the root cause the fork chased in `rb3dx_force_zero_commit`
(`src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:34-40`) -- whose own help text records that it
"did NOT fix the title OOM". It is worth re-testing that hypothesis with `AllocFixed` restored.

**Fix:** restore upstream `AllocFixed`. If the aliasing requirement is real (the comment cites
"MAP_SHARED file-backed mappings ... needed for virtual/physical address aliasing"), keep the
mprotect path but `memset` the range to zero on the commit path, and keep `MAP_PRIVATE` in the
fallback.

### C13. Profile XUID and gamertag changed for every title

`src/xenia/kernel/xam/user_profile.cc:33-34`

```c
xuid_ = 0xE00000000000BABEull;   // was 0xB13EBABEBABEBABE
name_ = "Player1";               // was "User"
```

The XUID is the key that title save containers and profile data are written under. Changing it
orphans every save any user of this build made previously, in every game, with no migration and
no warning. The reasoning in the comment (offline XUIDs have top nibble 0xE) is sound and worth
upstreaming; the compatibility break is not acknowledged anywhere.

**Fix:** either keep the change and add a release note plus a migration path, or gate it behind
`--offline_xuid` until it can be upstreamed with a proper profile-version bump.

### C14. `ObReferenceObjectByHandle` type-check semantics changed for every title

`src/xenia/kernel/xboxkrnl/xboxkrnl_ob.cc:88-121`

The fork replaced the `D###BEEF` sentinel comparison with a lookup of the `Ex*ObjectType`
export *variable address*, and added three new variable exports at
`src/xenia/kernel/xboxkrnl/xboxkrnl_module.cc:163-183` (`ExEventObjectType`,
`ExThreadObjectType`, `ExSemaphoreObjectType`, initialised to `0xD00BEEF0/1/2`). Titles that
previously passed the sentinel and matched now compare against a different value and get
`X_STATUS_OBJECT_TYPE_MISMATCH`. This is a plausible correctness fix, but it inverts an
existing contract across all titles with no compatibility fallback.

**Fix:** accept *either* the sentinel or the variable address during a transition period, log
which form the title used, and only then drop the old path.

### C15. A failed audio driver registration latches globally and disables all clients

`src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc:57, 73-74, 90, 106`

```c
static bool g_audio_driver_is_dummy = false;
...
g_audio_driver_is_dummy = true;
*driver_ptr = 0x41550000;                 // dummy handle, same base as real handles
```

A process-global (non-atomic) latch: once *any* `XAudioRegisterRenderDriverClient` fails, every
subsequent `XAudioUnregisterRenderDriverClient` and `XAudioSubmitRenderDriverFrame` becomes a
no-op -- including for clients that registered successfully. Games register multiple render
driver clients, so one transient failure silently mutes the rest. The dummy handle is also
`0x41550000` exactly, i.e. index 0, which collides with the first real client's handle.

**Fix:** make it per-handle. Reserve a distinct dummy index (e.g. `0x4155FFFF`) and test the
handle rather than a global flag.

### C16. Stubs registered for all titles that silently write guest memory or falsify results

Each of these is a new export registered unconditionally. Previously the title would have hit
"undefined extern" -- loud and diagnosable. Now it gets a plausible-looking wrong answer.

| Location | Behaviour |
|---|---|
| `xboxkrnl_rtl.cc:795-800` | `RtlCaptureContext` `memset`s **512 bytes** into a guest pointer with no size validation, then returns |
| `xboxkrnl_rtl.cc:802-807` | `RtlUnwind` does nothing -- SEH unwind silently no-ops for every title |
| `xboxkrnl_rtl.cc:809-817` | `__C_specific_handler` returns `ExceptionContinueSearch` always |
| `xboxkrnl_threading.cc:1604-1626` | `KeInitializeTimerEx` `memset`s 0x28 bytes; `KeSetTimer`/`KeCancelTimer` are lies that return "not queued" |
| `xboxkrnl_threading.cc:1630-1645` | `KeInitializeMutant` `memset`s 0x20 bytes; `KeReleaseMutant` returns 0 |
| `xboxkrnl_crypt.cc:704-714` | `XeKeysGetConsoleID` writes 5 bytes of `0x42` (`:708`) |
| `xbdm_misc.cc:100-109` | `DmGetSystemInfo` `memset`s 0x24 bytes into a guest pointer |
| `xboxkrnl_memory.cc:694-699` | `ExAllocatePoolWithTag` forwards to `SystemHeapAlloc` with no matching free -- unbounded leak |

The `memset` sizes are all guesses (`0x200`, `0x28`, `0x20`, `0x24`) with no bounds check
against the caller's buffer. `XeKeysConsolePrivateKeySign` (`xboxkrnl_crypt.cc:717-728`) is the
model to follow -- it explicitly declines to write the output buffer *because* the size is
uncertain, and says so in the comment.

**Fix:** for each, either implement it properly, or return an error status without touching
guest memory. Blind `memset` of a guessed struct size into a guest pointer is worse than the
unimplemented-export failure it replaced.

### C17. Silent page-table clamping in `BaseHeap`

`src/xenia/memory.cc:1086-1097` and `:1193-1197`

```c
uint32_t max_page = uint32_t(page_table_.size());
uint32_t clamped_end = std::min(end_page_number, max_page - 1);
for (uint32_t page_number = start_page_number; page_number <= clamped_end; ++page_number) { ... }
```

`AllocRange` now silently truncates the page-state loop and still returns success with
`*out_address` set. The caller believes it owns `size` bytes; the page table only records the
clamped prefix. `Release` has the same clamp at `:1193`. Both are silent (no log). This is the
one class of assert demotion the brief explicitly says to flag.

Note that `src/xenia/memory.cc:885` in the same file is a genuine off-by-one fix
(`end_page_number > page_table_.size()` -> `>=`) that should be split out and upstreamed on its
own -- it is the correct guard, and with it in place the clamps below become unreachable.

**Fix:** if `end_page_number >= page_table_.size()`, `XELOGE` and return false. Do not
half-succeed.

### C18. `PE Override` copies sections into guest memory with no bounds check

`src/xenia/cpu/xex_module.cc:1054-1160`

```c
std::memcpy(dest + sec->VirtualAddress, pe_data.data() + sec->PointerToRawData, copy_size);
```

`dest` is `TranslateVirtual(base_address_)`; `sec->VirtualAddress` and `SizeOfRawData` come
straight from the file with no validation against the module's image size. A mismatched or
truncated PE writes arbitrarily far past the module into guest (and eventually host) memory.
`fread`'s return value is unchecked and `ftell` could return -1. This is gated behind
`--pe_override` (default empty) so it is inert by default, which is why it is last in this
section -- but it is a file-driven arbitrary write.

**Fix:** validate `sec->VirtualAddress + SizeOfRawData <= image_size_` before each `memcpy`;
check `fread` and `ftell`.

---

## 2. Cleanup

Dead code, debug cruft, duplication. Grouped by file.

### `src/xenia/emulator.cc`

- `:154-158` -- `dc3_debug_memmgr_assert_nop_bypass` is defined and never read anywhere in
  `src/`. **Delete the cvar.**
- `:2298-2374` -- `try_bootstrap_gameplay`, 77 lines, never called. The comment at `:3089-3093`
  ("Gameplay bootstrap is disabled -- GamePanel::CreateGame blocks...") records the retirement.
  **Delete the lambda.**
- `:116-120, :1829, :1841-1844, :2300` -- `dc3_enable_gameplay_bootstrap` is dead with the
  above: its only read is inside the dead lambda, and `:1842` *writes a cvar at runtime from a
  JIT extern handler*. **Delete cvar, the `s_gameplay_setup_done` static, and the write.**
- `:532-543` + `:4448` + `:5115` -- `Dc3NuiReturn1Extern` is unreachable: it is selected only
  when `patch.insn0 == kLiR3_1`, and `kLiR3_1` appears in neither patch table.
  **Delete both.**
- `:141-148, :5083-5090` -- `dc3_guest_overrides` is inert; `const bool enable_guest_overrides
  = true;` hardcodes it and the cvar is read only to print "ignored". **Delete or restore the
  branch.**
- `:2853` -- `kContentMgrRefreshSynchronously` declared, never used (its call site was removed
  at `:2962-2968`). **Delete.**
- `:2137-2138` -- `auto* scr_obj = ...; (void)scr_obj;` leftover of a removed probe. **Delete.**
- `:1341-1422` (`--si_probe`, 82 lines) and `:878-962` (`--si_selftest`, 85 lines) both observe
  what `:1440` calls "the DEAD static .data cave"; `:2382-2632` (`--dc3_gameplay_probe`, 251
  lines) is retired `RndPropAnim::GetKeys` forensics per its own help text at `:128-134`.
  **~420 lines of retired forensics; move to a `dc3_probes.cc` or delete -- they should not be
  in the boot path.**
- `:4429-4505` -- 77 lines of pure comment with six speculative `TODO:` blocks about
  hypothetical Kinect / speech / SmartGlass emulation (`:4458, 4463, 4497, 4529, 4550, 4565,
  4595, 4643`). **Move the rationale to `docs/`, keep a two-line pointer.**
- `:2720-2746` -- a 4 MiB guest scan at 4-byte stride, run **twice**, each iteration calling
  `is_guest_readable` -> `LookupHeap` + `QueryRangeAccess`. ~1M heap lookups per pass, on the
  NUI callback thread, every time a nav target is resolved. **Cache the result or bound the
  scan.**
- `:2095-2117` -- `scan_for_literal` calls `is_guest_readable` at *every byte offset* of
  `.rdata`. **Hoist the readability check to the region, not the byte.**
- `:4304-4341, :4349-4361` -- the RB3 offline-join installer sweeps 0x82000000-0x83000000
  twice, the second sweep purely to log a `bl`-caller count. **Delete the diagnostic sweep.**
- `:1595, 1620-1625, 1663-1690, 1723-1731, 1743-1747, 1766-1769, 1795-1798, 1812-1817` --
  `Rb3dxUiProbeThread` logs informational output at `XELOGE`, every 2 s, including a full
  guest name-table dump (one line per entry). **Demote to `XELOGD`.**
- `:1053-1182` -- the alloc probe emits ~40 `XELOGE` lines per report x 64 reports, including
  six fixed code-window dumps at hardcoded addresses. **Demote and reduce.**
- `:1030-1040, :5719-5726` -- raw `xe::memory::Protect(...)` bypassing the heap bookkeeping and
  never restored, leaving a guest code page permanently writable and `QueryProtect`
  desynchronised. Everywhere else in the file correctly uses `heap->Protect` (`:5338, :5484,
  :5524`). **Use `heap->Protect` and restore.**

### `src/xenia/cpu/backend/x64/`

- `x64_seq_vector.cc:802, 1015, 1390, 1490, 2145, 2188, 2325, 2343` and
  `x64_sequences.cc:2470, 2484, 2525, 2543, 2999, 3080` -- fourteen copies of a dead null guard:

  ```c
  if (!src1_ptr || !src2_ptr) { std::memset(value, 0, sizeof(value)); ... }
  ```

  Every caller passes `e.lea(e.GetNativeParam(N), e.StashXmm(N, ...))` -- a stack address that
  is never null (see `x64_seq_vector.cc:859-864` and the ~20 identical sites). These add a
  branch to every emulated vector shift/pack/pow2 in every game. **Delete all fourteen.**
  (The surrounding `__m128i`-by-value -> `const vec128_t*` signature change is a legitimate
  SysV ABI fix and should stay -- see R2 in Rebase hygiene.)
- `x64_sequences.cc:1662` and `:1711` -- unexplained codegen change:
  `e.mulx(i.dest, e.edx, e.eax)` -> `e.mulx(i.dest, e.eax, e.eax)` (and the `rdx`/`rax`
  equivalent). No comment, no test. It *may* be fixing the `dst1 == dst2` undefined-result case
  when `i.dest == edx`, but it reintroduces the same problem when `i.dest == eax`, and the
  non-constant sibling branch at `:1664` was left inconsistent with the I64 version at `:1713`.
  **Either document it as a fix with a PPC test, or revert.**
- `x64_emitter.cc:604-616` -- `ResolveFunction` calls `ClassifyNonTextExecutableTarget`, which
  calls `Processor::GetModules()` (`src/xenia/cpu/processor.cc:208-215`), which
  **acquires the global critical region**. This now happens on the JIT resolve thunk for every
  title. `Processor::GetModules` also has a pre-existing bug the fork now exercises: it
  `resize`s to `modules_.size()` and then `push_back`s, returning a vector half full of nulls
  (the `if (!module ...)` guard at `x64_emitter.cc:221` masks it). **Gate the classifier behind
  the DC3 telemetry cvar; fix or avoid `GetModules`.**
- `x64_emitter.cc:642-650` -- `UndefinedCallExtern` string-compares every extern name against
  `"Rtl"` on the fatal path to emit an extra `XELOGW`. Harmless but is leftover
  page-0-hypothesis scaffolding. **Delete.**

### `src/xenia/cpu/`

- `ppc_scanner.cc:67-79, 124-140` -- the `consecutive_invalid >= 8` early termination and the
  `max_scan_address` heap clamp are ungated changes to function-boundary detection for every
  title. A legitimate function with 8 words of embedded data (jump tables, constant pools) now
  gets truncated. **Gate, or raise the threshold and require `furthest_target <= address`
  *and* no reachable branch beyond.**
- `ppc_hir_builder.cc:93-96` -- 1 MB function-size cap, ungated. **Log when it triggers; it is
  currently silent.**
- `ppc_opcode_lookup_gen.cc:18` -- `PPC_DECODER_MISS` lost its `assert_always()`. Less severe
  than C1 since `kInvalid` is handled downstream, but it is a silent demotion. **Add an
  `XELOGE`.**
- `xboxkrnl_rtl.cc:40-46` + `xboxkrnl_rtl.h:27-30` -- `g_rtl_enter_cs_count`,
  `g_rtl_init_cs_count`, `g_rtl_leave_cs_count` and their three accessors are exported in the
  header and read by nothing. **Delete** (along with the three `fetch_add` sites at `:472`,
  `:405`, `:610`).

### `src/xenia/kernel/`

Unconditional `XELOGI` added to hot or frequently-called paths, for all titles. All should be
`XELOGD` or gated:

- `xboxkrnl_io.cc:124-125` -- every `NtCreateFile`
- `xboxkrnl_io.cc:196-207` -- every `NtReadFile`, with a **non-atomic** `static uint32_t
  read_count` incremented from multiple guest threads (data race)
- `xboxkrnl_threading.cc:169-172` -- every `ExCreateThread`
- `xboxkrnl_threading.cc:559-570` -- every `NtSetEvent`, non-atomic static counter, and a
  hardcoded magic handle `0xF80000EC` whose own successor comment at `:571-580` says handle
  numbers are not stable across runs. **This block is superseded by `rb3_trace_shutdown`
  directly below it -- delete it.**
- `xboxkrnl_modules.cc:54-58, 92-93, 113-114` -- `XexGetModuleHandle`, `XexLoadImage`
- `xboxkrnl_misc.cc:27-31, 34-37` -- `EtxProducerRegister`/`Unregister`
- `xboxkrnl_threading.cc:1604, 1612, 1619, 1630, 1638` -- the Ke timer/mutant stubs
- `xbdm_misc.cc:45-52` -- `DmIsDebuggerPresent`, non-atomic static counter
- `xam_content.cc:111, 130`, `xam_content_device.cc:115, 138`, `xam_notify.cc:23, 38, 60, 100`,
  `xam_ui.cc:285, 580, 621, 646`, `xam_user.cc:124, 573`, `xam_nui.cc:44, 53`,
  `xam_input.cc:122, 127`, `xam_info.cc:400, 408, 415`
- `xboxkrnl_rtl.cc:476-535` -- the `crt_critical_section_diagnostics` block uses a non-atomic
  `static uint32_t cs_enter_count` (`:515`) from multiple guest threads. Cvar-gated, so
  low priority, but it is a race. Same shape at `xboxkrnl_threading.cc:621` where
  `static std::vector<uint64_t> seen_sigs` is mutated unsynchronised and grows unbounded.

Other:

- `xboxkrnl_threading.cc:361, 956, 999` -- three sites hardcode `thread->thread_id() == 6` as
  "the main thread". **Use `kernel_state()->GetExecutableModule()`'s main thread, or a named
  helper.**
- `xam_info.cc:360-420` -- `GetLocalTime`, `GetSystemTime`, `GetTickCount`, `OutputDebugStringA/W`,
  `RtlOutputDebugString` are kernel32-shaped names registered in the **XAM** table.
  `FillSystemTime` (`:360`) writes 16 bytes without a null check on `out_ptr`.
  **Add the null check; consider whether these belong in xboxkrnl.**
- `xam_enum.cc:60-91` -- the overlapped `NO_MORE_FILES` -> `SUCCESS` conversion is ungated for
  all titles. The comment block is excellent (it records the exact regression and the commit
  that caused it) -- **keep the comment, add a cvar or leave a note that this is deliberate
  all-title behaviour.**
- `shim_utils.h:399-415` -- value printing removed from `AppendParam` for `lpdword_t`,
  `lpqword_t`, `lpfloat_t`, `lpdouble_t`. The reason (dereferencing write-watched pages
  deadlocks the MMIO handler) is real, but every title's export trace lost its parameter values.
  **Use a non-faulting read (`LookupHeap` + `QueryProtect`) instead of dropping the value.**

### `src/xenia/dc3_nui_patch_resolver.cc`

- `:2393, :2408, :2422` -- absolute developer-machine paths compiled into the binary:

  ```c
  "/home/free/code/milohax/dc3-decomp/config/373307D9/symbols.txt",
  "/home/free/code/milohax/xenia/docs/dc3-boot/dc3_nui_fingerprints.txt",
  "/home/free/code/milohax/dc3-decomp/build/373307D9/xenia_dc3_patch_manifest.json",
  ```

  **Move to cvar defaults resolved relative to the storage root, or require the cvar.**
- `:27` (`dc3_runtime_telemetry.cc`) -- `dc3_runtime_telemetry_include_ppc_words` is defined and
  never read. **Delete.**

### `src/xenia/gpu/`

- `vulkan_pipeline_cache.cc:97` -- `std::filesystem::path("/tmp/claude") / ...` hardcoded, and
  referenced in a user-visible cvar help string at `vulkan_command_processor.cc:69`.
  **Use the storage root.**
- `command_processor.cc:565-570` -- `assert_always()` on primary-ringbuffer packet failure
  demoted to `XELOGE` + `break`, ungated. It logs, so it is within the brief's tolerance, but
  the `break` (rather than upstream's continue-anyway) changes flow for every title.
  **Note the flow change explicitly in the comment or gate it.**
- `graphics_system.cc:254-258` -- `assert_not_null(thread)` demoted to a silent `return`.
  **Add a log.**
- `gpu/premake5.lua:23-56` -- `xenia-gpu-headless` hardcodes a 17-file source list. Any new
  `.cc` added to `src/xenia/gpu/` silently fails to compile into the headless build.
  **Use `local_platform_files()` with a `XE_HEADLESS_BUILD` filter, matching the other
  headless projects.**

### Repo root

- `analyze_poolalloc.py` (452 lines) and `test_headless.sh` (113 lines) sit at the repository
  root; both hardcode `/home/free/code/milohax/...` paths (`analyze_poolalloc.py:10`,
  `test_headless.sh:9`). **Move to `tools/`, parameterise the paths.**
- `.gitignore:104` adds `/stdout` -- something writes a file literally named `stdout` at the
  repo root. **Find and fix the writer rather than ignoring the artefact.**
- `tools/dc3_crt_bisect.sh:36-38` hardcodes `XENIA_DIR` and `XEX_PATH` with no `${VAR:-}`
  fallback, unlike its five sibling scripts which all do it correctly. **Match the siblings.**

---

## 3. Structural

Title-specific logic living in shared files. Line counts are approximate.

### Where DC3/RB3 code lives

| File | Fork lines | What |
|---|---:|---|
| `src/xenia/dc3_hack_pack.cc` | 6,946 | DC3 guest overrides, pool allocator, PPC bytepatching |
| `src/xenia/emulator.cc` | ~4,900 | see breakdown below (upstream file was 845 lines; now 5,764) |
| `src/xenia/dc3_nui_patch_resolver.cc` | 2,793 | manifest/signature resolution |
| `src/xenia/app/emulator_headless.cc` | 2,148 | headless loop + an ~800-line inline GDB RSP server |
| `src/xenia/cpu/milo_trace.cc` | 1,642 | JIT trace capture |
| `src/xenia/hid/nop/nop_input_driver.cc` | +1,120 | scripted input playback |
| `src/xenia/gpu/vulkan/vulkan_command_processor.cc` | +1,035 | headless capture + render debugging |
| `src/xenia/dc3_runtime_telemetry.cc` | 549 | telemetry sink |
| `src/xenia/dc3_hack_pack_skeleton.cc` | 364 | skeleton-path overrides |

**The most consequential structural fact:** `src/xenia/premake5.lua:16` builds `xenia-core`
from `files({"*.h", "*.cc"})`. `src/xenia/` now contains **four DC3-specific translation units**
(`dc3_hack_pack.cc`, `dc3_hack_pack_skeleton.cc`, `dc3_nui_patch_resolver.cc`,
`dc3_runtime_telemetry.cc` -- ~10,600 lines) alongside the six upstream core files. They are
compiled into and linked by *every* Xenia binary, including `xenia-app`. Moving them to
`src/xenia/dc3/` with its own premake project, linked only by the headless target, is the
single highest-leverage structural change and costs nothing but include paths.

Hardcoded title IDs appear in 45 places: `emulator.cc` (35), `app/emulator_headless.cc` (4),
`xboxkrnl_memory.cc` (4), `xboxkrnl_threading.cc` (2). A `constexpr uint32_t kTitleDc3 =
0x373307D9; kTitleRb3dx = 0x45410914;` in one header would make every gate greppable.

### `emulator.cc` breakdown

`emulator.h` is **byte-identical to upstream** -- all 4,900 lines are in the anonymous namespace
or inline in `CompleteLaunch`, which is why none of it is testable in isolation.

| Lines | ~n | Block | Emulator internals needed | Extractability |
|---|---:|---|---|---|
| 100-408 | 309 | 41 `DEFINE_*` for `dc3_*`/`rb3*`/`si_*` | none | **trivial** -> `dc3_flags.cc`, `rb3dx_flags.cc` |
| 415, 56-58 | 4 | `using namespace xe::dc3;` + 3 DC3 includes | -- | -- |
| 417-510 | 94 | `AccessOpName`, `MaybeWriteDc3CrashSnapshotJson` | `Memory`, `Exception`, `XThread`, `PPCContext` | **easy** -> general `crash_snapshot.cc` |
| 512-543 | 32 | 3 trivial NUI return-value externs | `PPCContext` | **easy** -> `dc3_hack_pack.cc` |
| 545-650 | 106 | RB3DX binary trace sink + 4 file globals | none | **easy** (extract globals as one unit) |
| 652-747 | 95 | MemFree/MemAlloc entry+return probes | `PPCContext`, `Memory` | **easy** |
| 749-1183 | 435 | MemAlloc entry probe + `--si_selftest` + `--si_load_dll` | **`Processor::Execute`**, `xe::memory::Protect` | **hard** -- calls back into the guest from a hook |
| 1185-1237 | 53 | offline-join override | `PPCContext`, `Memory` | **easy** |
| 1239-1820 | 582 | four passive host sampler threads | **`Memory*` only** | **easiest large win** -> `rb3dx_probes.cc` |
| 1822-3212 | 1,391 | `Dc3NuiSequencerExtern` (see below) | everything | **hardest**, but liftable as-is |
| 4007-5747 | ~1,580 | inline blocks in `CompleteLaunch` | the `Dc3HackContext` set | **low-risk, high-value** |

**`Dc3NuiSequencerExtern` (`:1822-3212`)** is registered as the override for
`NuiSkeletonGetNextFrame` but only ~114 lines of it are a NUI stub. The rest drives UI screen
transitions (`:2157-2296`), scans `.rdata` for string literals (`:2037-2127`), runs a song-catalog
repair with `SystemHeapAlloc`/`Free` (`:2843-3087`), and advances the song clock (`:3095-3209`).
It needs `Memory`, `KernelState` (processor, object table, executable module *and its PE section
table*), `Processor::Execute` guest re-entry, `XThread::EnqueueApc`, and `Memory::SystemHeapAlloc`
-- i.e. a "title hook interface" exposing all of that is just `Emulator*`. **The realistic move
is to lift it verbatim into `dc3_nui_sequencer.cc`; its signature is already
`(PPCContext*, KernelState*)`, so it can move today with no interface design. That is 1,391
lines, 28% of the fork's additions to the file.**

**`CompleteLaunch` (`:4007-5747`)** is 1,741 lines, ~1,580 of them title-specific. Everything
in it except the milo-trace session block (`:4113-4139`, which is all-title and belongs in core)
needs only `memory_`, `processor_`, `kernel_state_`, `module`, `content_root_`, and `title_id_`
-- **exactly the `Dc3HackContext` struct the fork already builds at `:5230-5237` and `:5262-5266`.**
Extend that into a `TitleLaunchContext`, add `Dc3ApplyLaunchHooks(ctx)` and
`Rb3dxApplyLaunchHooks(ctx)`, and `CompleteLaunch` drops to ~200 lines with two calls.

Within it, `:4506-4771` is **266 lines of two hardcoded patch tables** (`patches`,
`decomp_patches`). These are pure data and should be a JSON file loaded by the existing manifest
machinery, which already exists at `:4391-4427`.

### Other structural notes

- `src/xenia/app/emulator_headless.cc:106-900` -- an ~800-line GDB RSP server class
  (`Dc3GdbRspHeadlessListener`) defined inline in the headless app, with an `#ifdef __linux__`
  stub twin at `:904-914`. It has its own socket handling, thread, breakpoint map, and target
  XML generation. **Belongs in `src/xenia/debug/` next to `dc3_gdb_rsp_protocol.h`** -- and once
  there, it is a generally useful Xenia feature, not a DC3 one. Rename accordingly.
- **Five duplicated premake projects** (`xenia-core-headless`, `xenia-kernel-headless`,
  `xenia-gpu-headless`, `xenia-gpu-null-headless`, plus `xenia-headless`) exist solely to
  compile the same sources with `XE_HEADLESS_BUILD` defined. That doubles build time for the
  whole tree. `XE_HEADLESS_BUILD` is only consulted in 6 source files (`emulator.cc` x7,
  `xam_ui.cc` x13, `xam_nui.cc` x3, `graphics_system.cc` x3, `null_graphics_system.cc` x2).
  **A premake configuration or filter would do the same job without duplicating five projects.**
- `src/xenia/app/xenia_headless_main.cc:70` defines `DEFINE_string(gpu, "null", ...)` while
  `src/xenia/app/xenia_main.cc:66` defines `DEFINE_string(gpu, "any", ...)`. Two cvars with the
  same name and different defaults in two binaries. It links (separate targets) but is a trap
  for anyone reading config docs. **Rename the headless one or share one definition.**

---

## 4. Rebase hygiene

Diffs against upstream files that make a future rebase harder than it needs to be.

### R1. License-header reformatting in four upstream files

`src/xenia/gpu/graphics_system.cc:1-8`, `src/xenia/gpu/null/null_graphics_system.cc:1-8`,
`src/xenia/kernel/xam/xam_nui.cc:1-8`, `src/xenia/kernel/xam/xam_ui.cc:1-8`

The leading space was stripped from every line of the standard BSD header:

```
- ******************************************************************************
+******************************************************************************
```

Eight guaranteed conflict lines at the very top of four files, for zero benefit. **Revert.**

### R2. Two large, genuinely valuable non-title-specific fixes are buried in title-specific commits

These are *not* slop -- they are good work that will be lost or mangled at the next rebase
because they are indistinguishable from the DC3 churn around them:

- **SysV AMD64 ABI support in the x64 thunks** --
  `src/xenia/cpu/backend/x64/x64_backend.cc:421-427, 440-446, 457-463, 473-479, 526-536,
  592-598` (`#if XE_PLATFORM_LINUX` arms in `EmitHostToGuestThunk`, `EmitGuestToHostThunk`,
  `EmitResolveFunctionThunk`) plus the `__m128i`-by-value -> `const vec128_t*` signature change
  across `x64_seq_vector.cc` and `x64_sequences.cc`. This is a real Linux port fix.
- **`Memory::QueryProtect` on POSIX** -- `src/xenia/base/memory_posix.cc:119-135` replaces
  upstream's `return false;` stub with a `/proc/self/maps` parse. `mmio_handler.cc:456` now
  actually checks the result. Also a real fix.

**Suggestion:** cherry-pick both onto a clean branch off `upstream/master` and open PRs. They
carry no DC3 coupling, and getting them upstream removes them from the rebase surface
permanently. (Note the `AllocFixed` change in the same file is *not* in this category -- see C12.)

### R3. Vendored `third_party` file edited in place

`third_party/half/include/half.hpp:1042`

```c
-inline half operator "" _h(long double value) { ... }
+inline half operator""_h(long double value) { ... }
```

A one-line C++23 literal-suffix warning fix applied directly to a vendored library. Any vendor
bump silently reverts it and the warning returns with no record of why. **Move to a
`patches/half-literal-suffix.patch` applied at setup time, or upstream it to half.**

### R4. Whitespace-only reflow in upstream files

Measured as `git diff` line count vs `git diff -w` line count:

| File | Extra churn |
|---|---:|
| `src/xenia/gpu/null/null_graphics_system.cc` | 9 lines |
| `src/xenia/gpu/graphics_system.cc` | 7 |
| `src/xenia/cpu/backend/x64/x64_seq_control.cc` | 7 |
| `src/xenia/kernel/xam/xam_nui.cc` | 7 |
| `src/xenia/kernel/xam/xam_ui.cc` | 7 |
| `src/xenia/base/threading_posix.cc` | 4 |
| `src/xenia/cpu/ppc/ppc_hir_builder.cc` | 3 |

Small in absolute terms (~45 lines total across the tree, which is genuinely good discipline for
177 commits). Most of it is R1. Worth a single `clang-format`-free cleanup pass on those files
so the rebase surface is only real changes.

### R5. Gratuitous single-line edits

- `src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:550` -- a blank line added inside
  `NtCreateEvent` with no other change to the function.
- `src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:955-975` -- `KeWaitForSingleObject` changed
  from `return xeKeWait...(...)` to `auto result = ...; return result;` purely to hang tracing
  off it, leaving a misaligned continuation line at `:967`.
- `src/xenia/cpu/ppc/testing/premake5.lua:71` -- "No newline at end of file" fixed while adding
  two links.

None of these matter individually; together they are three extra conflict hunks.

---

## 5. Do these first

Ordered by (risk removed) / (effort). Items 1-6 are correctness for other titles; 7-10 are the
structural moves that make everything after them cheaper.

1. **Restore the unimplemented/invalid-instruction diagnostics.**
   `ppc_emit-private.h:25`, `ppc_hir_builder.cc:177, 197-203`, `ppc_opcode_lookup_gen.cc:18`.
   Four lines. Without this the emulator cannot report the most common porting bug in any game.
   *(C1)*

2. **Gate the MMIO soft-fault paths behind cvars and title checks.**
   `mmio_handler.cc:542-560` (read soft-fault, all titles) and `:485-501` (DC3 address range
   hardcoded in the shared handler). *(C2, C3)*

3. **Gate the null-call tolerance behind one cvar.**
   `x64_seq_control.cc:187-260, 279-393`, `x64_emitter.cc:567-647, 737-819`. The branches are
   already isolated; this is mostly adding `if (cvars::tolerate_null_guest_calls)`. *(C4)*

4. **Restore `AllocFixed` on POSIX, then re-test the RB3DX OOM.**
   `base/memory_posix.cc:88-104`. Guest commits currently return non-zeroed pages for every
   title, which is very likely the bug `rb3dx_force_zero_commit` was built to chase and failed
   to fix. Fixing this may retire that cvar entirely. *(C12)*

5. **Bound and validate every guest-stack walk; delete the dead `if (!ptr)` guards.**
   `emulator.cc:3800-3869`, `xboxkrnl_threading.cc:378-390, 585-608`. `TranslateVirtual` never
   returns null -- five guards in the fork currently pretend otherwise, and one of them runs
   inside the exception handler. *(C9)*

6. **Own the detached probe threads.**
   `emulator.cc:4249, 4260, 4269, 4281, 4385` + `vulkan_pipeline_cache.cc:486`. A `vector<thread>`
   member, an `atomic<bool>` stop flag, and joins in `TerminateTitle()` / `~Emulator()`.
   ~20 lines; removes a guaranteed shutdown use-after-free. *(C10)*

7. **Gate `Dc3MaybeCleanStaleContentCache` behind a cvar.**
   `dc3_hack_pack.cc:6684`, called unconditionally from `emulator.cc:4416`. One line, prevents
   real save-data loss. *(C11)*

8. **Move the four DC3 TUs out of `xenia-core`.**
   `src/xenia/{dc3_hack_pack,dc3_hack_pack_skeleton,dc3_nui_patch_resolver,dc3_runtime_telemetry}.cc`
   -> `src/xenia/dc3/` with its own premake project. ~10,600 lines stop being linked into
   `xenia-app`. Mechanical; no logic changes. Do this before any other structural work.

9. **Lift `Dc3NuiSequencerExtern` verbatim into `dc3_nui_sequencer.cc`, then extract the
   `CompleteLaunch` blocks via the existing `Dc3HackContext`.**
   `emulator.cc:1822-3212` moves as-is (its signature is already right), and `:4007-5747`
   collapses to two `*ApplyLaunchHooks(ctx)` calls. Together that is ~3,000 lines out of
   `emulator.cc`, taking it from 5,764 back toward ~1,500.

10. **Delete the confirmed-dead code and split out the two upstreamable fixes.**
    Dead: `dc3_debug_memmgr_assert_nop_bypass`, `dc3_runtime_telemetry_include_ppc_words`,
    `try_bootstrap_gameplay` + `dc3_enable_gameplay_bootstrap` (~90 lines), `Dc3NuiReturn1Extern`,
    `dc3_guest_overrides`, `kContentMgrRefreshSynchronously`, the 14 dead vector null-guards,
    the three unread `GetRtl*CsCount` accessors, and the superseded `NtSetEvent` trace block.
    Upstreamable: the SysV ABI thunk work and POSIX `QueryProtect` (R2), plus the `BaseHeap`
    off-by-one at `memory.cc:885`. Getting those three onto `upstream/master` permanently
    shrinks the rebase surface.

---

### Categories with nothing to report

- **Copy-paste duplication introduced by fork changes:** beyond the 14 dead vector null-guards
  (which are a template artefact rather than hand-copied) and the five duplicated premake
  projects, none found. `emulator_headless.cc` does *not* meaningfully copy `xenia_main.cc`;
  it is a genuinely separate, simpler loop.
- **Raw `new`/`delete` where the codebase uses `unique_ptr`:** none found in the fork's
  additions. `emulator.cc` in particular is clean here.
- **`HACK`/`XXX`/`FIXME` markers added by the fork:** none found. All fork-added markers are
  `TODO:`, concentrated in the `xboxkrnl` stub bodies (C16) and the DC3 NUI comment header
  (`emulator.cc:4429-4505`).
- **Changed defaults on pre-existing upstream cvars:** none found. All 50 fork-added cvars are
  new names; six default to enabled (`dc3_crt_skip_nui`, `dc3_game_screen_real_goto`,
  `dc3_guest_overrides`, `dc3_nui_enable_signature_resolver`, `dc3_persist_render_state`,
  `headless_report_boot`) and all six are inside DC3/headless-only code paths.

---

### One thing worth preserving

`emulator.cc:2637-2646` (the `NOTE (2026-06-02)` recording the confirmed `LoadMgr::Poll` crash
from guest re-entry on the NUI thread), `xam_enum.cc:60-91` (which names the exact commit that
caused the RB3DX splash hang and why the fix must not be applied to the sync path), and
`xboxkrnl_memory.cc:20-40` (which records that `rb3dx_force_zero_commit` *did not* fix the OOM)
are the three most valuable comments in the fork. They are documented negative results. Any
extraction must carry them verbatim.
