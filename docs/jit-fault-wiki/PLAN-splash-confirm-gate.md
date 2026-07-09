# PLAN — RB3DX splash `Confirm → StartOvershell` gate: fix `XamEnumerate` sync-path regression, reach `main_hub` → instrument-select

**Status:** FIX LANDED & VERIFIED (2026-07-09), commit `9096dd4d0`. The core
objective — the `XamEnumerate` sync-path regression (`a224a6846`) — is FIXED:
the splash `{saveload_mgr is_idle}` gate is cleared and RB3DX now advances
`splash_screen → first_time_calibration`. The instrument-select acceptance test
(T4) was **NOT** reached: the boot now stalls one flow further on, at the
**first-boot `first_time_calibration` → `cal_audio_screen`** interactive A/V
latency calibration, which fixed-time scripted `A` presses + null audio cannot
complete headless. That is a downstream first-boot flow, not the fix. See
"Results (2026-07-09)" below.
**For:** a follow-on multi-agent Workflow (planning → implementation → review
segmentation; see §6).
**Prereqs:** none beyond the current `headless-vulkan-linux` tree. All prior
fixes (`fb864e3e` zero-page, `b803faab1` circuit-breaker, `e248d624c` GetInfo,
`a2b655c8d` splash diagnostics) are in.
**Cross-links:** [BRIEF-main-hub-load-stall.md](BRIEF-main-hub-load-stall.md)
(§"Fable investigation #3" = the evidence this plan executes on) ·
[WORKSTREAM-rb3-on-xenia-bringup.md](WORKSTREAM-rb3-on-xenia-bringup.md) ·
[09-rb3dx-title-to-menu.md](09-rb3dx-title-to-menu.md) ·
[CRASH-REPORT-main-hub-oom.md](CRASH-REPORT-main-hub-oom.md) · [INDEX.md](INDEX.md)

---

## 1. Confirmed root cause (do not re-derive)

RB3DX sits on `splash_screen` forever because the splash state machine's
`ActivateSaveLoad → StartOvershell` hop polls `{saveload_mgr is_idle}`
(RB3DX splash script, decoded at
`/tmp/rb3dx-hub-investigate/dta/ui_splash_gen_splash.dtb.dta:256-262`), and
`SaveLoadManager` never reaches idle: its auto-load's save-container search —
`MemcardXbox::FindValidUnit`, **100% byte-verified vs retail**
(`rb3-xenon src/system/os/Memcard_Xbox.cpp:464-510`, called from
`MemcardMgr::ThreadCall_SearchForDevice`,
`src/system/meta/MemcardMgr_Xbox.cpp:295`) — spins forever in

```c
while (XEnumerate(h1c0, &buffer, 0x134, &dw1bc, nullptr) == 0) { … }
```

because **this fork's `xeXamEnumerate` rewrites `X_ERROR_NO_MORE_FILES` →
`X_ERROR_SUCCESS` + 0 items for BOTH completion paths**
(`src/xenia/kernel/xam/xam_enum.cc:50-58`, added by fork commit `a224a6846`
for DC3's *overlapped* `XGetOverlappedResult` semantics). An exhausted
**synchronous** enumerator therefore returns `0` forever → infinite guest loop
on the Memcard worker thread → `is_idle` never true → `attempt_to_add_user` /
`OvershellSlot::AddUser` / `NetSession::AddLocalUser` never run → `main_hub`
never loads. Sign-in (a) and pad/user-slot (c) hypotheses are REFUTED
(BRIEF #3). Input delivery and consumption are PROVEN (movie-skip control
experiment; no-input boot stalls identically).

## 2. The fix (primary, general, DC3-safe)

**File:** `src/xenia/kernel/xam/xam_enum.cc`, `xeXamEnumerate`.

**Change:** apply the `NO_MORE_FILES → SUCCESS/0-items` conversion **only in
the overlapped path**, restoring stock-Xenia synchronous semantics.

Concretely: the conversion currently lives inside the shared `run` lambda.
Move it (or gate it) so that:

- **synchronous path** (`items_returned != nullptr`, `overlapped_ptr == 0`):
  returns `e->WriteItems(...)`'s result verbatim — i.e. `X_ERROR_NO_MORE_FILES`
  (`0x12`) on exhaustion, `*items_returned = 0`. This is upstream Xenia's
  behavior and what the retail guest code expects (`FindValidUnit`'s loop exits
  on any non-zero; it also explicitly handles `0x12` from
  `XContentCreateEnumerator`).
- **overlapped path** (`overlapped_ptr != 0`): keep the existing conversion
  bit-for-bit (`extended_error = 0; length = 0; return X_ERROR_SUCCESS;`) — this
  is the real-360-faithful `XGetOverlappedResult` behavior the DC3 fix targeted.

Sketch (one clean way; implementer may restructure equivalently):

```cpp
auto run = [e, buffer_ptr](uint32_t& extended_error,
                           uint32_t& length) -> X_RESULT {
  X_RESULT result;
  uint32_t item_count = 0;
  if (!buffer_ptr) {
    result = X_ERROR_INVALID_PARAMETER;
  } else {
    result = e->WriteItems(buffer_ptr.guest_address(),
                           buffer_ptr.as<uint8_t*>(), &item_count);
  }
  extended_error = X_HRESULT_FROM_WIN32(result);
  length = item_count;
  return result;
};

if (items_returned) {
  // Synchronous: propagate X_ERROR_NO_MORE_FILES to the caller (stock
  // behavior; RB3's MemcardXbox::FindValidUnit while-loop depends on it).
  ...
} else if (overlapped_ptr) {
  // Overlapped: on real 360 the overlapped completes SUCCESS/count=0 instead
  // of propagating ERROR_NO_MORE_FILES through XGetOverlappedResult (DC3
  // handles only 0 / 0x65B). Wrap `run` to keep that conversion HERE ONLY.
  ...
}
```

Implementation details (from the Opus verification pass):
- `XEnumerator::WriteItems` returns `X_ERROR_NO_MORE_FILES` whenever
  `item_count_ - current_item_ == 0` and `current_item_` is never reset
  (`src/xenia/kernel/xenumerator.cc:63-66`) — exhaustion is permanent, so the
  converted-SUCCESS return repeats forever; that is the infinite loop.
- The overlapped machinery is sound and must stay unchanged:
  `CompleteOverlappedDeferredEx` seeds `result=0x3E5` then completes on the
  dispatch thread ~100 ms later via `CompleteOverlappedEx` →
  `XOverlappedSetResult/Length` (`src/xenia/kernel/kernel_state.cc:748-800`,
  `:696-720`; `XOVERLAPPED` layout `src/xenia/xbox.h:176-201`). The observed
  successful rbdxcache/globaloptions mounts prove the dispatch thread runs.
- In the synchronous branch, set `*items_returned = (result == X_ERROR_SUCCESS)
  ? item_count : 0` and return the raw result.
- Optional belt-and-braces: gate the sync-path restoration behind a
  **default-ON** cvar (e.g. `--xam_enumerate_sync_hw_accurate`) for instant
  A/B during verification. Title-gating is unnecessary and would be wrong —
  the sync bug bites any title doing synchronous enumerate-until-done.

**No title gating needed** — this is an emulator-correctness fix restoring
upstream behavior on the sync path while keeping the fork's overlapped fix.

**DC3-safety is by construction AND by parity:** DC3's cache enumerate is
overlapped (`dc3-decomp/src/system/utl/CacheMgr_Xbox.cpp:79`) — that path is
untouched — and DC3 ships the **identical synchronous `FindValidUnit` loop**
(`dc3-decomp/src/system/os/Memcard_Xbox.cpp:496`); it merely doesn't exercise
synchronous exhaustion at its current boot depth. Restoring `0x12` on
exhaustion is what that loop *wants* — the change protects DC3 rather than
risking it. T5 proves it empirically anyway.

**Why not a guest-side / DTA-side `is_idle` stub instead?** The native port
stubs `saveload_mgr is_idle`→1 (its `SaveLoadManager` can't run at all). On
Xenia the real `SaveLoadManager` CAN run — the only broken link is the
emulator's enumerate contract. Fixing the contract keeps the guest faithful
(profile save search completes "not found", saveload finishes its real state
machine) and fixes every other sync-enumerate consumer for free. A guest
override would also need fragile signature-scanning (RB3DX is relocated vs
`band.exe` — see BRIEF #2). **Fallback only** (if §5 verification exposes a
second, deeper saveload hang): title-gated (`0x45410914`) default-off guest
override forcing `SaveLoadManager::IsIdle()` true, or a DTA-level
`saveload_mgr is_idle` interpose — mirror of the native port's proven stub
(`rb3/native/src/rb3_game_input.cpp:1212-1224`).

## 3. Residual risks / watch-items (verify during implementation, in order)

After the enumerate fix, `FindValidUnit` returns `kMCFileNotFound` (only
`rbdxcache`/`globaloptions` exist; no profile-save container). Downstream
watch-items, cheapest evidence first:

1. **No-save routing in `MemcardMgr_Xbox`/`SaveLoadManager`** (rb3-xenon
   `MemcardMgr_Xbox.cpp:380` `case kMCFileNotFound:` and the Wii-complete
   `rb3/src/band3/meta_band/SaveLoadManager.cpp` as oracle): expect the
   auto-load to conclude "no save" and go idle (possibly after an autosave-
   creation prompt on first boot). If a device selector or message box fires:
   `XamShowDeviceSelectorUI` headless already completes immediately with dummy
   device `0x00000001` (`src/xenia/kernel/xam/xam_ui.cc:509-521`), and
   `XamShowMessageBoxUIEx` is dispatched headless similarly — check which
   button index the headless dispatcher picks; if it picks a "cancel" that
   loops the prompt, that is the next (small) gate.
2. **G1 confirm at the splash:** with `is_idle` true, one START on pad 0 must
   flip `splash_state` Entered→ActivateSaveLoad→StartOvershell (the splash's
   `BUTTON_DOWN_MSG` remaps Start→Confirm→`SELECT_MSG` on the focused —
   hidden but selectable — `start.btn`). Input consumption is already proven
   (movie skip). Note: it is UNRESOLVED whether the spinning thread
   (`F8000100`) is a Memcard worker or the game logic thread; if the latter,
   the pre-fix spin was ALSO starving input processing, and the enumerate fix
   clears both gates at once. If SELECT unexpectedly fails post-fix, suspect
   the hidden-button focus path; evidence: no `{text.grp set_showing FALSE}`
   effect and no `saveload_mgr activate` re-issue.
3. **Overshell join eligibility:** `OvershellPanel::RefreshJoinableUsers` gates
   joins on `ConnectedControllerType() != kControllerNone`
   (`rb3/src/band3/meta_band/OvershellPanel.cpp:504-551`). Real retail bytes
   accept Xenia's standard-gamepad caps as `kJoypadAnalog`
   (`ReadSingleXinputJoypad` @`0x8251EB24`: unknown subtype → analog;
   only `dwPacketNumber==-1` → none), and `JoypadResetXboxPC` associates
   users↔pads at init — so this SHOULD pass. If the slot join silently no-ops,
   this is the place to probe.
4. **The NetSession local join:** once `AddLocalUser` finally runs, with
   `mQNet==0` the real `IsHost()` already returns true (BRIEF #2), so the
   offline host branch should fire `AddUserResultMsg(1)` synchronously and
   `overshell_allowing_input(TRUE)` advances WaitOvershell→EndOvershell→
   `main_hub_screen`. The existing `--rb3dx_offline_join` override
   (default-off, title-gated, self-locating via IsHost signature scan @runtime
   `0x823E1700`) is the ready-made mitigation if the join stalls after all.
   The existing `--rb3dx_ui_probe` shows IsHost hit counts + session state.
5. **First-boot flow after EndOvershell:** `{ui push_screen
   first_time_calibration}` fires if `!get_has_seen_first_time_calibration`
   (splash.dtb.dta:388-393) — a dialog screen with `opt1.btn`/`opt2.btn`;
   scripted `A` selects an option (opt1 routes via cal_welcome, opt2 goes to
   `main_hub_screen`). Budget nav inputs for it.

## 4. Implementation task list

> **Status (2026-07-09): T1–T3 + T5 DONE and VERIFIED; T4 PARTIAL; T7 DONE.**
> The fix landed as commit `9096dd4d0`. See "Results (2026-07-09)" after the
> task list for the per-task evidence and the new downstream gate.

- [ ] **T0 — (optional, 2 lines) decisive pre-fix confirmation.** Add a
  temporary rate-limited `XELOGI` (or atomic counter dumped by the headless
  status loop) in `xeXamEnumerate`'s synchronous branch when it converts
  `NO_MORE_FILES` for an already-exhausted handle. Boot RB3DX unfixed: an
  unbounded call rate on one handle after the last content op = the infinite
  loop observed directly. Skippable — the static evidence is already strong;
  do it if the reviewer wants a live smoking gun. Remove before commit.
- [x] **T1 — the fix.** DONE (`9096dd4d0`). Edit `src/xenia/kernel/xam/xam_enum.cc` per §2. Keep
  the overlapped conversion + its comment; add a comment on the sync path
  citing this plan + `MemcardXbox::FindValidUnit`. Build
  (`xb build --config Checked` / the fork's normal build; binary
  `build/bin/Linux/Checked/xenia-headless`).
- [x] **T2 — RB3DX boot A/B.** PASS. Boot with the standard flags
  (`--target=/tmp/rb3dxboot/default.xex --protect_zero=false --gpu=vulkan
  --local_user_count=2 --fault_spin_limit=4096 --rb3dx_ui_probe=true`) and NO
  scripted input, 150 s. **Expect:** content ops now CONTINUE past the
  `user=0 flags=4096` enumerator (device-state / content-create /
  profile-settings calls appear); `--rb3dx_ui_probe` still shows
  `splash_screen` (no input yet) but SaveLoadManager work completes. Grep for
  new `XamShow*` calls (watch-item §3.1).
- [x] **T3 — splash advance.** PASS. Re-run with scripted input
  (`--scripted_input="20s:START@0,30s:START@0,40s:A@0,50s:A@0,60s:A@0"`,
  re-tuned from the observed movie-end time; START must land AFTER the splash
  is current). **Acceptance:** `--rb3dx_ui_probe` shows `curScreen` leaving
  `splash_screen` (→ `first_time_calibration` dialog or `main_hub_screen`),
  and the probe's `session` dump shows the local-user join (mUsers non-empty
  and/or IsHost hits if `--rb3dx_offline_join` armed for observability).
- [~] **T4 — reach main_hub + instrument-select.** PARTIAL (blocked one flow
  downstream — see Results). Extend the nav script
  (title→hub is START; menu selects are A; P2 join `START@1`; per
  `rb3-verify/scripts/rb3dx_title_to_guitar.txt`, re-timed on the working
  boot). Capture frames (`--dump_frames_path --headless_capture_interval=40`).
  **Milestone:** instrument-select screen reached headless = the acceptance
  test for this whole plan. (Same-instrument A/B is the NEXT phase, staged in
  [09](09-rb3dx-title-to-menu.md) §L4.)
- [x] **T5 — DC3 non-regression (MANDATORY).** PASS — DC3-SAFE. Boot retail DC3
  (`dc3-decomp/orig/373307D9/default.xex`) headless twice (fix ON = the only
  build; compare vs pre-fix binary or `git stash`-free A/B via a second build
  dir): `--gpu=null --stub_nui_functions=true --headless_timeout_ms=20000
  --dc3_runtime_telemetry_enable=true`. **Expect:** identical milestone
  sequence (session_begin → nui_patch_apply_complete →
  headless_timeout_reached), `SIGSEGV=0`, VdSwap count within jitter — same
  matrix as the `b803faab1` verification ([09](09-rb3dx-title-to-menu.md)
  §L5). DC3's enumerate consumers use the OVERLAPPED path (preserved
  bit-for-bit), so no divergence is expected; prove it anyway.
- [x] **T6 — clean-TU5 spot check (cheap, optional).** PASS — unchanged. The sync-path
  restoration is title-global; a clean-TU5 boot
  ([08](08-boot-to-menu.md)) should be unchanged (it self-exits on the LOLZ
  ark regardless).
- [x] **T7 — docs + commit.** DONE (this pass). Update
  [BRIEF-main-hub-load-stall.md](BRIEF-main-hub-load-stall.md) (#3 section →
  "FIXED, commit <sha>"), [INDEX.md](INDEX.md) status banner,
  [WORKSTREAM](WORKSTREAM-rb3-on-xenia-bringup.md) §1/§4.D/§8, and the memory
  file. Commit message should cite `a224a6846` as the regressing commit.
  Consider an upstream-Xenia note: upstream lacks the overlapped conversion
  entirely; only the fork needs this split. Also decide disposition of the
  still-uncommitted `--rb3dx_offline_join`/probe extensions in `emulator.cc`
  (keep as diagnostics; they are default-off).

## 5. Verification matrix (summary)

| Check | Command sketch | Pass criterion |
|---|---|---|
| Sync enumerate contract | T2 boot, grep content ops | I/O continues past `user=0 flags=4096` enumerator; no infinite silence |
| saveload idle | T3 boot + probe | splash leaves `splash_screen` on START |
| Join completes | T3 probe `session` dump | mUsers/join observed; `overshell_allowing_input` fires (screen advances) |
| main_hub | T4 frames + probe | `curScreen=main_hub_screen`, legible frame |
| Instrument-select | T4 nav | screen reached; **acceptance test** |
| DC3-safe | T5 A/B | identical milestones, SIGSEGV=0 |

## 6. Workflow segmentation (for the ultracode/Workflow author)

- **Stage P (planning, 1 agent, Opus):** read this plan + BRIEF #3; validate
  the §2 sketch against current `xam_enum.cc`; produce the exact patch +
  the re-timed nav script (movie-end timing from
  `/tmp/rb3dx-splash-gate/noinput-boot.log` probe cadence ≈2 s/sample,
  transition at sample [10] ≈ 20 s). Checkpoint JSON to
  `$CLAUDE_JOB_DIR/tmp/splash-confirm-gate/plan.json`.
- **Stage I (implementation, 1-2 agents, Opus):** T1–T4 (one agent), T5–T6
  (parallel second agent once T1's build exists). Checkpoint each boot's
  log path + verdict. Honest reporting: if a §3 watch-item fires, STOP and
  document which one — do not stack speculative fixes in one pass.
- **Stage R (review, 1 agent):** re-run T3 acceptance + T5 DC3 A/B from the
  committed tree; verify the docs (T7) tell the truth; confirm no harness
  files (`command_processor.cc`, `nop_input_driver.*`, `xam_input.cc`) were
  modified; sign off or bounce to Stage I with the failing evidence.

**Constraints carried over (HARD):** DC3-safety mandatory; no XDK/copyrighted
downloads; harness files untouched; no `git add -A`/stash; commit only files
the workflow changed.

---

## Results (2026-07-09) — fix landed `9096dd4d0`, splash gate CLEARED, T4 blocked one flow downstream

**Bottom line:** the plan's core objective is **FULLY VERIFIED**. The
`XamEnumerate` sync-path regression (`a224a6846`) is fixed and RB3DX is no longer
pinned on `splash_screen`. The instrument-select acceptance test was **not**
reached — but the new blocker is a first-boot **calibration flow** downstream of
the fix, not the fix itself.

The fix (`src/xenia/kernel/xam/xam_enum.cc`) matches §2 verbatim: the
`NO_MORE_FILES → SUCCESS/0` conversion now lives in a `run_overlapped` wrapper on
the overlapped path only; the synchronous path returns `WriteItems`' result
verbatim.

| Task | Verdict | Evidence |
|---|---|---|
| **T2** sync enumerate contract | **PASS** | No-input, 150 s: the `user=0 device=0x0 type=1 flags=4096` enumerator (`MemcardXbox::FindValidUnit`) fires EXACTLY ONCE (`t2-noinput.log:9071`), handle immediately Removed, no spin; **16,595** runtime ops (VdSwap presents) continue AFTER it; sync exhaustion returns `NO_MORE_FILES(0x12)`. `grep flags=4096` count = 1. Pre-fix this returned SUCCESS/0 forever (infinite Memcard-worker loop). |
| **T3** splash advance | **PASS** | UI probe `sample[22]=splash_screen → sample[23]=first_time_calibration`; the splash `Confirm→StartOvershell` gate fired on scripted `START@0`; scripted input confirmed delivered (6 events, `0x0010`=START / `0x1000`=A). |
| **T4** main_hub + instrument-select | **PARTIAL** | Reached `splash → first_time_calibration → cal_welcome_screen → cal_audio_screen` (`sample[35]`), then STALLED ~74 s to timeout. `main_hub_screen`/`instrument_screen` appear ONLY in the maindir screen-registry dump, NEVER as `curScreen`. Input IS delivered+consumed on `cal_audio_screen` (`XamInputGetState` user=0 `0x1000` registered repeatedly during the stall) — so input is live; the interactive `cal_audio_panel` latency-calibration logic is the gate. 386 real 1280×720 P6 frames captured. |
| **T5** DC3 non-regression | **PASS — DC3-SAFE** | Retail DC3 booted headless twice (`--fault_spin_limit` 4096 and 0). Identical milestone sequence (`session_begin → dc3_nui_patch_block_begin → dc3_nui_patch_apply_complete → headless_timeout_reached`, ~19.74 s) vs the `b803faab1` baseline. SIGSEGV plateaus at **4** in both with an IDENTICAL fault signature to baseline (pre-existing benign recovered fault, byte-identical in the `b803faab1` log — NOT a regression). FAULT LIVELOCK=0. DC3's enumerate consumers only exercise the overlapped path (import-table registration only; no sync-exhaustion signature) → fix empirically inert for DC3. |
| **T6** clean-TU5 spot check | **PASS** | RB3 clean-TU5 nodd boot self-exits ~18015 ms via its own `App::Shutdown()` exactly as documented in [08](08-boot-to-menu.md); SIGSEGV=0; fix inert for this lane. |

**The new downstream gate (watch-item for the next pass):** first-boot
`first_time_calibration → cal_audio_screen`. `cal_audio_panel` is an interactive
A/V-latency calibration (measure output-vs-input lag by having the player tap to
a click). Headless with **null audio** and **fixed-time scripted `A` presses**,
that measurement cannot complete, so the screen never advances. An attempted
T4b opt2-skip (`DOWN`+`A` on the `first_time_calibration` `dialog_small_panel`,
per §3.5 watch-item 5 opt2→`main_hub`) **regressed** to staying on
`first_time_calibration` — the skip path is not trivially scriptable. Per the
HARD "don't stack speculative fixes" constraint, no further nav/code was stacked.

Next-pass candidates (cheapest first): (i) set
`get_has_seen_first_time_calibration` true (a saved-settings / globaloptions
prime, or a title-gated default-off guest override) so first boot skips
calibration entirely → straight to `main_hub`; (ii) drive the `cal_audio` skip
button correctly (needs the exact `opt2.btn` focus/nav sequence, not blind
`DOWN`+`A`); (iii) provide a headless calibration-complete signal. All three are
downstream of, and independent of, the now-fixed enumerate contract.

**Logs:** `/tmp/wf-splash-confirm-gate/{t2-noinput,t3-scripted,t4-nav,t4b-nav}.log`,
frames `/tmp/wf-splash-confirm-gate/t4-frames/`, DC3
`/tmp/wf-splash-confirm-gate/dc3/{a_default,b_disabled}.{log,jsonl}`,
checkpoints `rb3dx.json` / `dc3.json` / `finalize.json`.
