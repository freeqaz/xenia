# 08 — Boot to Main Menu (RB3 TU5, headless Xenia)

Phase goal: get clean TU5 RB3 to a rendered, navigable `main_hub` headless, then
drive two controllers to the same instrument and reach gameplay.

Cross-links: [01 symptom & evidence](01-symptom-and-evidence.md) ·
[03 guest-code analysis](03-guest-code-analysis.md) ·
[06 root cause](06-root-cause.md) · [07 fix & verification](07-fix-and-verification.md)

---

> **⚠️ CORRECTION (L3, GPU-capture lane, authoritative on this point):** L4's
> conclusion below that the black frame is a "GPU frontbuffer-capture tracking
> bug" — that the capture keeps reading the old/wrong resolve target after the
> frontbuffer base changes — is **REFUTED**. The capture path *already* follows
> the guest's current VdSwap frontbuffer: at frame 600 it reads
> `frontbuffer=0x1CE30000` (the NEW base, not the old 0x1D1C8000), uploads it
> **fresh from guest RAM** (`decision=UPLOAD`, line 4526), and it comes back
> **0% non-zero because the game drew nothing into it** — the deferred-draw
> flush counter is frozen at 5 (no draws issued after frame 500). This is a
> **DRAWS-STOPPED game-logic stall, not a capture bug.** No `command_processor.cc`
> change is warranted. See the **L3 section at the bottom of this page** for the
> full evidence. L4's content/save/movie refutations (sections 2–3 below) still
> stand; only its capture-lane handoff is wrong.

## L4 — Content / Save + Intro-Movie: NOT the blocker (both REFUTED)

**Verdict: neither a kernel/XAM content-or-save stub, a blocking `XamShow*`
dialog, nor the Bink intro movie is what stalls the jump to `main_hub`.** The
guest advances normally and renders real full-screen scene content, then stops
issuing draws. **(The "GPU frontbuffer-capture tracking bug" this section
originally blamed is refuted by L3 — see the correction box above.)**

Evidence source: `/tmp/jitfault-wf/v1-A-input.log` (scripted 2-controller run,
`--protect_zero=false --local_user_count=2`, killed by `--headless_timeout_ms=120000`).

### 1. The decisive signal — frame-capture verdicts

`RSTAB: CAPTURE frame=N ... nonzero_pct=P verdict=...` lines show the game DID
progress past the splash and rendered a full scene, then the capture lost the
frontbuffer:

| Frame | frontbuffer base | nonzero_pct | verdict | interpretation |
|------:|------------------|------------:|---------|----------------|
| 100 | 0x1D1C8000 | 6 | HUD_ONLY | ESRB / autosave splash |
| 200 | 0x1D1C8000 | 6 | HUD_ONLY | still splash |
| 300 | 0x1D1C8000 | **100** | **SCENE** | **advanced — full scene rendered** |
| 400 | 0x1D1C8000 | **100** | **SCENE** | scene still rendering |
| 500 | 0x1D1C8000 | **100** | **SCENE** | scene still rendering |
| 600 | **0x1CE30000** | **0** | HUD_ONLY | frontbuffer base changed → capture reads stale/wrong buffer |
| 700 | 0x1CE30000 | 0 | HUD_ONLY | same wrong base |

Interpretation: the guest reached a **100%-non-zero SCENE** (intro movie or the
menu itself) for frames 300–500. At ~frame 600 the guest flipped its swap
frontbuffer to a **new base_page (0x1D1C8000 → 0x1CE30000)** — exactly what a
screen transition (movie → menu, or splash → hub) does — and the capture path
kept resolving the old/wrong resolve target, yielding 0% non-zero. Guest threads
keep advancing across the whole window (thread-6 LR advances in hang dumps; **759
`VdSwap` presents** logged, ring pointers monotonically advancing
`BDC902EC→…→BF617AE8`). **This is a live present loop, not a stall.**

→ The real open item belongs to the **GPU-capture lane**: teach the readback in
`command_processor.cc` to follow the guest's current `VdSwap` frontbuffer
base_page instead of latching the frame-100/300 resolve target. That is what
produces the black frames from ~600 on; it is not a content/save/movie stall.

### 2. Content / save — no runtime call, no blocking dialog

Every relevant XAM export is **implemented and instrumented with `XELOGI`** in
this fork (`src/xenia/kernel/xam/`), yet **runtime invocation count = 0** for all
of them in the entire log:

```
XamShowMessageBox        0   XamContentCreateEnumerator  0
XamShowDeviceSelector    0   XamContentCreateEx          0
XamShowDirtyDisc         0   XamContentGetDeviceState    0
XamShowSignin            0   XamUserReadProfileSettings  0
XamEnumerate             0
```

(The `XamShow*`/`XamContent*` lines at v1-A-input.log:474–542 are the **IAT
import table listing** — imports the game *links*, printed once at load — not
runtime calls. Runtime kernel calls appear as `i> F<handle> Name: …`.)

- No `XamShowDeviceSelectorUI` / `XamShowSigninUI` / `XamShowMessageBoxUI` /
  `XamShowDirtyDiscErrorUI` ever fires → **no blocking headless dialog exists to
  hang on.** And they *can't* block: all route through `xeXamDispatchHeadless`,
  which returns `X_ERROR_SUCCESS` immediately (`xam_ui.cc:509`), and
  `XamShowDirtyDiscErrorUI` (`xam_ui.cc:523`) logs the guest LR if ever raised —
  it was not.
- No `XamContentCreateEnumerator`/`XamContentCreateEx`/`GetDeviceState` →
  **the game never even enumerates save data in this window.** Savegame/title
  `45410914` is registered (v1-A-input.log:166/213/845) but not read here, so a
  "save enumeration wrong-return" cannot be the stall.
- Sign-in completed cleanly: `XamUserGetSigninState user0→1, user1→1`
  (1619–1620). The system-UI notification opened and **closed** on its own:
  `XNotifyGetNext dequeued id=0x00000009 param=0x00000001` then `param=0x00000000`
  (955/957) = `XN_SYS_UI` open→close; `id=0x0000000A param=1` (959/961) =
  `XN_SYS_SIGNINCHANGED`; `0x12`/`0x13` = input-device change. Auto-dismiss of
  the sign-in overlay already works.

### 3. Intro movie — never entered at runtime

- `Movie::Init` (0x82A4B350) and `BinkMovieSys::Init` (0x83438C9C) appear **only
  inside the harness "Function probes (present pipeline)" symbol-presence dump**
  (v1-A-input.log:1642–1643 and repeats), where **every** entry — including
  RB3-range 0x82/0x83 symbols like `main()`, `App::App()`, `LoadMgr::Init` —
  prints `NOT COMPILED`. That block is the harness checking whether hack-pack
  bodies are resident (they are not, for this RB3 process); it is **not** a live
  call trace. No `Bink`/`Mv*`/`Movie` symbol is ever *invoked*.
- No movie/XMA playback activity: only the standard boot-time `XMA Decoder`
  host thread (line 139, spun up unconditionally) and two idle
  `XMA: Write to unknown register (0601)` pokes (1011–1012). No streaming, no
  "movie done" wait.
- Positively, the frames 300–500 `SCENE 100%` captures show visual content
  advancing on screen — whatever plays (attract/menu) is **rendering and moving
  forward**, not frozen on a decode wait.

Native-port cross-check (rb3 memory `intro_movie_screen`): on native the Bink
intro is a `<video>`/host shim and RB3's attract/intro is input-skippable
(`RB3_INTRO_SECS`). If a future run *does* show the guest entering a Bink wait,
the skip is a scripted controller press — but no such wait is present here.

### Ranking of this lane's candidate blockers (all refuted)

1. Blocking `XamShow*` dialog — **REFUTED** (0 invocations; headless dispatch is
   non-blocking by construction).
2. Save-enumeration wrong-return — **REFUTED** (0 enumeration calls in-window).
3. Intro-movie decode wait — **REFUTED** (movie APIs never called; scene renders
   at 100% for frames 300–500).

### Concrete unblock (hand-off, not L4)

The stall the user sees is the **frontbuffer base_page change at ~frame 600**
(`0x1D1C8000 → 0x1CE30000`) that the capture/readback in
`command_processor.cc` does not follow (`RSTAB … decision=SKIP_STALE`,
`verdict=HUD_ONLY`, 0% non-zero). Fix belongs to the GPU-capture lane: on each
`VdSwap`, re-derive the capture frontbuffer from the *current* swap parameters
rather than reusing the latched resolve target, so post-transition frames
(menu/hub) are captured. No kernel/XAM stub or cvar change is warranted from L4.

---

## L3 — GPU Capture-Path: is the black real, or does the game draw where readback can't see it?

**Verdict: DRAWS-STOPPED. Real black.** The game stops issuing GPU draw commands
after ~frame 500 and parks on presenting a frontbuffer page it never rendered
into. The capture path is armed and correct; it faithfully reads the exact
`frontbuffer_ptr` the game hands to VdSwap, uploaded fresh, and it is genuinely
black. **This is an L1/L2 game-logic stall — no capture-path fix will make menu
pixels appear, because there are none being drawn.**

Evidence: `/tmp/jitfault-wf/v1-A-input.log` (`--protect_zero=false
--local_user_count=2`, `headless_capture_interval=100`, normal deferred headless
mode).

### How the capture path works (this fork)

Headless readback lives in **`src/xenia/gpu/vulkan/vulkan_command_processor.cc
IssueSwap()`** (presenter is null in headless), swap texture built in
**`src/xenia/gpu/texture_cache.cc RequestSwapTexture()/LoadTextureData()`**.
Two-phase deferred capture, one frame per `headless_capture_interval_`:

- **Render frame** (the swap before a capture): `next_is_capture` arms
  `deferred_draws_enabled_=true` and logs `"Enabled deferred draws for render
  frame (next is capture)"` (vulkan_command_processor.cc:1447-1451). Every PM4
  draw/copy issued that window is queued into `deferred_draws_` instead of
  executing live.
- **Capture frame**: `IssueSwap()` calls `FlushDeferredDraws()` (:1369-1371),
  logging `"FlushDeferredDraws: executing N deferred draws"` and incrementing
  `deferred_flush_count_` (the `flush#` in the RSTAB line). Then
  `RequestSwapTexture()` on the guest `frontbuffer_ptr` from VdSwap, reads back,
  counts non-zero, logs `RSTAB: CAPTURE frame=… flush#=… frontbuffer=0x…
  nonzero_pct=… verdict=…` (:1717-1723).

### What the two log tokens mean (in THIS code)

- **`verdict=HUD_ONLY` vs `SCENE`** (vulkan_command_processor.cc:1721-1723) is
  *only a label on the readback result*: `nonzero_pct > 15` → `SCENE`, else
  `HUD_ONLY`. It suppresses nothing. `HUD_ONLY` at 0% just means "captured swap
  image was (almost) all black."
- **`decision=SKIP_STALE` vs `UPLOAD`** (texture_cache.cc:650-656,
  `LoadTextureData`): if the swap texture at `base_page` is neither
  `base_outdated` nor `mips_outdated`, the cache returns the existing texture
  without re-reading guest RAM. **`SKIP_STALE` is correct and harmless here** —
  the page genuinely didn't change. It is *not* the cause of the black: the
  first time 0x1CE30 appears (frame 600) it is `UPLOAD` (fresh read) and *still*
  0% non-zero.

### The decisive timeline

| Capture frame | VdSwap ptr | flush# | nonzero | verdict |
|---:|---|---:|---:|---|
| 100 | 0x1D1C8000 | 1 | 6%   | HUD_ONLY (splash appearing) |
| 200 | 0x1D1C8000 | 2 | 6%   | HUD_ONLY |
| 300 | 0x1D1C8000 | 3 | 100% | SCENE |
| 400 | 0x1D1C8000 | 4 | 100% | SCENE |
| 500 | 0x1D1C8000 | 5 | 100% | SCENE |
| **600** | **0x1CE30000** | **5** | **0%** | HUD_ONLY |
| **700** | **0x1CE30000** | **5** | **0%** | HUD_ONLY |

Render frames (99…499) presented `0x1CE30000`, capture frames (100…500)
presented `0x1D1C8000` — a normal **double-buffer flip**; the splash lived in
`0x1D1C8000`. At frame 599 the ptr is already `0x1CE30000` and **stays there
through 600/700**: the game stopped flipping and parked on `0x1CE30000`.

### Three independent signals: no draws after frame 500

1. **`FlushDeferredDraws: executing N` is stuck at 5** — last flush at frame 500
   (log line 4013); whole-log `grep -c` = 5. `deferred_flush_count_` only
   increments inside `FlushDeferredDraws`, which only runs when `deferred_draws_`
   is non-empty → **zero draws queued for the render windows before frames 600
   and 700.**
2. **Capture was armed anyway** — `"Enabled deferred draws for render frame"` at
   lines 4519 (pre-600) and 5001 (pre-700). `deferred_draws_enabled_=true` was
   set; any draw the game issued *would* have been captured. It queued nothing.
3. **Zero replay + no frame-timing draws** — `RSTAB2: REPLAY_DRAW/REPLAY_COPY`
   after frame 500 = 0; frame-timing `"… N draws"` lines stop at frame 499 (128
   draws) — 599/699 emit none because `headless_draw_count_ == 0`.

The `0x1CE30000` page at frame 600 was read **fresh** (`RSTAB: LOAD
base_page=0x1CE30 base_outdated=true … decision=UPLOAD`, line 4526) → **0%
non-zero**. The game presented a frontbuffer it never rendered into. Frame 700
is `SKIP_STALE` (line 5006) because nothing touched it in between.

### Why "capture reads the wrong buffer" (L4's handoff) is impossible here

- The capture reads **exactly the guest `frontbuffer_ptr` VdSwap passed**
  (`0x1CE30000` at frame 600 — the NEW base, already followed), uploaded fresh,
  genuinely black. It does not latch the old resolve target.
- The "menu is resolved into EDRAM / the other buffer and we present the wrong
  one" theory requires draws/resolves to exist. **None do** after frame 500:
  nothing writes `0x1CE30000`, `0x1D1C8000`, or EDRAM. PM4 DRAW packets would
  flow through the same command processor that unconditionally records into
  `deferred_draws_` while armed; the flush stays empty forever.
- Format/swizzle (fmt=6, host_swizzle=0xA0A) is exonerated: the same path read
  **100% SCENE** at frames 300/400/500 with those same formats. A swizzle bug
  cannot selectively zero only the post-splash frames.

### Handoff

This is **L1/L2**. The game halts its render loop after the ESRB/autosave
splash: guest threads keep advancing (thread-6 LR walks
`82B8C6AC→8285B078→8275CFD8`) but the App present path emits no geometry —
likely blocked on savegame/content mount or a sign-in/flow gate before
`main_hub`, or spinning in a non-drawing state. Investigate guest-logic
(`App::DrawRegular` / flow state), not the capture path. Leave
`command_processor.cc` harness edits as-is.

Optional confirmation (not required): capture the *other* buffer `0x1D1C8000` at
frame 700 — expect the stale splash still resident, proving both buffers are
frozen and only the presented pointer changed.

---

## L2 — Guest-Progress Trace: what is the main thread doing in the black window?

**Verdict: the main thread is ALIVE in the normal per-frame present/vsync loop —
NOT deadlocked, NOT in a content/save kernel wait, NOT in an intro-movie decode
wait. It advances past the ESRB/autosave splash into a post-splash screen state
that draws no geometry and loads no files.** This confirms and *narrows* L3's
L1/L2 handoff: the stall is a **game flow-state park between the autosave splash
and the first drawn UI screen**, with the App still ticking frames.

Evidence: `/tmp/jitfault-wf/v1-A-input.log`. Guest addresses resolved against
`rb3-xenon/config/45410914/symbols.txt` (all TU5 entries are address-only
`fn_XXXXXXXX`; functionally characterized here via flat-image disassembly of
`_tu5probe/clean/band_clean_tu5.exe` [`file_off = VA − 0x82000000`] + the
emulator's own `NtWait`/`ExCreateThread` instrumentation — the 8001 Ghidra
service holds only the Wii binaries, so no C++ names exist for 0x82xxxxxx).

### The decisive signal — the steady LR *is* the VdSwap vsync wait

The main thread's LR settles at **0x82844CF8** for hang-dump snapshots 4–6 (the
black window). That address is not a busy loop — it is the **return address of an
infinite `NtWaitForSingleObject` on the graphics swap event**:

```
v1-A-input.log:1102  MainThread NtWait #1 handle=0xF80000D0 timeout=-999 type=2 guest_lr=82844CF8
v1-A-input.log:1103  NtSetEvent #9 tid=14 handle=0xF80000D0        <- render thread signals it
v1-A-input.log:1104  MainThread NtWait RETURNED handle=0xF80000D0 result=0x0   <- woken, never times out
```

- `handle=0xF80000D0` is an XObject event **created at Vd graphics init**
  (`Added handle:F80000D0` line 1050, right after `VdInitializeEngines`/
  `VdSetGraphicsInterruptCallback` at 967–968). It is the **VdSwap / vblank
  present-sync event**.
- `timeout=-999` = infinite wait; **`result=0x0` every time = signaled, never a
  timeout** → no hang. 18 of the main thread's waits are on `0xF80000D0`, 2 on
  `0xF80000F4` (an init-phase sync); **those are the only two objects the main
  thread ever blocks on in the whole run.**
- The signaller is **tid=14** (`ExCreateThread start=0x82742968`, line 1100),
  which is also the thread that issues every `VdSwap` (`VdSwap #12 tid=14 …`,
  line 1182 onward). **tid=14 = the RB3 render/present (GfxThread); tid=6 = the
  App/main thread that blocks on its swap event each frame.** Classic
  double-buffer producer/consumer, turning normally (759 `VdSwap`, 716 `XE_SWAP`,
  158 main-thread wait returns, all `result=0x0`).

So during the black window the main thread is doing exactly one thing: **running
the frame loop and parking on the swap event between frames.** It is not parked
on a content/save/movie/signin object.

### Main-thread LR progression across the six hang dumps

| Snap (log line) | LR | region / activity | phase |
|---|---|---|---|
| 1 (1554) | 0x82B8C6AC (fn_82B8C6A8) | deep C-runtime/JIT-trampoline frames (0x427DC89E, 0x402B1110) | early init, pre-splash |
| 2 (2495) | 0x8285B078 (fn_82859ns…→ fn @0x8285A8A8) | in `RtlEnterCriticalSection` region, VdSwap active (regs: r31=0x422ADD80 RTL_CS) | App update, ESRB splash (frame ~200) |
| 3 (3226) | 0x8275CFD8 (fn @0x8275CF28) | entering a critical section, VdSwap active, JIT sample guest=0x8283604C | App update, clear-to-scene (frame ~300) |
| 4 (3732) | **0x82844CF8** | **NtWait(0xF80000D0, ∞) — present/vsync wait**; JIT IP samples all `guest 0x00000000` (blocked in host kernel) | steady present loop (frame ~450+) |
| 5 (4228) | 0x82844CF8 | same present/vsync wait | black window |
| 6 (4710) | 0x82844CF8 | same present/vsync wait | black window |

The `guest 0x00000000` JIT IP samples at snapshots 4–6 confirm the guest PC is
inside the host `NtWaitForSingleObject` (not executing guest code) — i.e. parked
on the swap event, woken, presents, re-parks. Snapshots 1–3 (real guest
backtraces, critical-section + VdSwap activity) are the App genuinely running its
update/draw during the ESRB splash and the clear-to-(13,0,13) phase; by snapshot
4 it has settled into pure present-wait because **the current screen issues no
draws** (see L3: flush count frozen at 5).

### Worker threads — all parked idle, no load in flight

Every other thread is blocked in the **same engine event-wait helper family**
(fn ~0x82844CA0 / 0x828450E0), i.e. idle thread-pool workers, not spinning:

- tid 8,9 — worker pool, entry **0x8286C520**, LR 0x8286C5BC, parked.
- tid 11,12 — worker pool, entry **0x82C57480**, LR 0x82844CF8 (`guest_lr` of the
  same wait helper), parked.
- tid 10 — subsystem worker, entry **0x82BF3B60**, LR 0x82BF3B2C, parked.
- tid 7 — worker, entry **0x8252A3B0**, LR 0x82845144, parked (its stack bc8
  chain 0x8252A370→0x8284D6DC routes through the worker thread-body prologue at
  **0x8284D6A0**, the common `lr_bc8` seen on every worker stack).
- The `0xBEBEBEBE`/`0xBCBCBCBC` fill words in every worker backtrace are
  uninitialized stack past the parked frame — consistent with threads sitting in
  a shallow wait, not deep in work.

### No load, no content wait — it is a *flow* park, not a *load* park

- **Zero `NtCreateFile`/`NtReadFile` after frame 500** (log line 4050 onward) →
  no ARK/`.milo`/asset streaming is in progress. LoadMgr is idle; the worker pool
  is idle. So the App is **not** mid-load waiting for assets to finish.
- Main thread blocks **only** on the swap event (`0xF80000D0`) in this window —
  never on a content/save/signin object (cross-checks L4: 0 runtime XAM calls).
- The `App::App()`/`Movie::Init`/`UIManager::GotoFirstScreen` etc. at
  0x82…/0x83… in the log are the harness symbol-presence probe (all print
  `NOT COMPILED`), **not** a live call trace — ignore per COMMON CONTEXT.

Because the present loop turns every frame while the screen draws nothing and no
kernel wait is pending, whatever the post-splash screen is waiting on is a
**condition polled inside the per-frame update** (a flow gate / `GotoFirstScreen`
precondition / an event the App checks and finds not-ready), not a blocking OS
object. The frontbuffer flip 0x1D1C8000 → 0x1CE30000 at ~frame 600 is the App
entering that next (empty-drawing) screen node.

### Answer to the lane's four questions

1. **Named function per key LR** — no C++ names exist for TU5 (address-only
   symbols; Ghidra 8001 has only Wii images). Functional identities:
   `0x82844CF8` = return site of the infinite `NtWait(0xF80000D0)` **present/vsync
   sync** (engine frame-present + shared thread event-wait helper at ~0x82844CA0);
   `0x82845144` = sibling event-wait helper; `0x8284D6A0` = worker thread-body
   prologue; `0x8286C520`/`0x82C57480`/`0x82BF3B60` = idle worker-pool entries;
   `0x82742968` (tid 14) = **render/present thread** (issues VdSwap, signals the
   swap event); `0x8285A8A8`/`0x8275CF28` = App per-frame update fns active during
   the splash phase.
2. **Main-thread current activity** — the **normal per-frame present loop**:
   render (no geometry) → VdSwap (tid 14) → `NtWaitForSingleObject(swap event
   0xF80000D0, ∞)` → woken `result=0x0` → repeat. Alive, ~vsync-paced.
3. **Top-level state node** — a **post-ESRB-splash screen** (frontbuffer flipped
   to 0x1CE30000) that draws nothing; the App flow is parked *before* the first
   rendered UI screen (attract/`main_hub`), not inside a movie or a load.
4. **Before/during/after intro movie** — effectively **at the boundary AFTER the
   ESRB/autosave splash, before any content-bearing screen.** Not a Bink decode
   wait (movie APIs never invoked; no XMA streaming). The screen transition
   happened; the destination node produces no draws.

### Most likely reason for no visible output

The App has advanced past the ESRB/autosave splash and entered the next screen
node, but that node **issues no draw geometry** while the App keeps ticking the
present loop. With loaders idle and no kernel wait pending, the block is a
**flow-state gate polled per frame that never satisfies** (a `GotoFirstScreen` /
attract / hub precondition) — a guest game-logic condition, not an emulator
capture bug (L3) and not a kernel/XAM/content/movie stub (L4). Next step is an
L1 flow-state probe: instrument the App's per-frame flow node / `GotoFirstScreen`
path to see which screen is active and which condition it is re-checking.


---

## L1 — Decisive experiment: slow-load vs true stall → **it is NEITHER; the guest EXITS the title at ~19–20s (timeout-independent)**

**Verdict: the post-splash black is NOT a permanent stall and NOT a slow-load
that eventually reaches `main_hub`. It is a brief (~3 s) no-draw window that ends
in a GUEST-INITIATED TITLE EXIT at ~19–20 s. The `main_hub` never renders. This
run also CORRECTS the premise carried by L2/L3/L4 that the run was "killed by
`--headless_timeout_ms=120000`" — it was not; it self-exits well before any
timeout.**

Evidence: `/tmp/rb3menu-wf/l1-longrun.log` + `/tmp/rb3menu-wf/l1-frames/`
(this lane's long run: `--headless_timeout_ms=600000`,
`--headless_capture_interval=100`, `--protect_zero=false --local_user_count=2`,
same scripted 2-controller input as v1-A). Checkpoint:
`/tmp/rb3menu-wf/l1-longrun.json`.

### The decisive control — a 5× longer timeout changes nothing

| Run | `headless_timeout_ms` | last Thread Status Report | last `VdSwap #` | teardown starts | ends with |
|---|---:|---:|---:|---|---|
| Reference `v1-A-input.log` | 120000 | **18014 ms** | #700 | `Removed handle:F80000E8` | `terminate called` / core |
| **This lane `l1-longrun.log`** | **600000** | **18015 ms** | #700 | `Removed handle:F80000E8` | `terminate called` / core |

Two runs, timeouts 5× apart, **byte-identical exit timing (~18–20 s, same swap
count, same teardown sequence).** The `600000ms` value was applied (log line 151:
`Running with 600000ms timeout...`). Therefore the exit is **NOT** the headless
timeout. Nothing is waiting on the clock — the guest quits itself. A longer
timeout cannot and does not help.

### It is a *guest*-initiated shutdown, not a host abort

At the transition out of the present loop:

```
l1-longrun.log:5069  NtSetEvent #2048 tid=6 handle=0xF80000EC obj=0x7f0be84752c0   <- App main thread runs GUEST code
l1-longrun.log:5070  MainThread NtWait RETURNED handle=0xF80000E4 result=0x0
l1-longrun.log:5071  Removed handle:F80000E8 for ...XMutantE                        <- orderly kernel teardown begins
     ...            Removed handle: XThread / XEvent / XTimer / XNotifyListener ...
l1-longrun.log:5089  terminate called without an active exception                   <- LAST line (xobject.cc handle-leak assert, benign)
```

`tid=6` is the App/main thread (per L2). It is **executing guest code
(`NtSetEvent`)** to signal a shutdown event to its worker threads — i.e. the game
is running its own teardown path, not parked in the "infinite" present-wait. The
orderly `Removed handle:` cascade (all kernel objects unregistered) runs FIRST,
and `std::terminate` fires LAST during final cleanup — the known
`xobject.cc:52` handle-leak assert, a benign teardown artifact, **not** the
primary cause. No `SIGSEGV`, `last_fault=0x0`, `last_rip=0x0` on every status
report through 18 s. **No new guest crash.**

### Frame timeline — content DID appear, then black, then exit

Reading the actual PNGs (`/tmp/rb3menu-wf/l1-frames/view_*.png`):

| Frame | ~time | frontbuffer | non-zero | what is ON SCREEN (from the PNG) |
|------:|------:|-------------|---------:|----------------------------------|
| 0100 | 2.6 s | 0x1D1C8000 | 6% | **ESRB splash**: "Online Interactions and Music Downloads Not Rated by the ESRB" + the autosave-symbol/save disclaimer on black |
| 0400 | 10.5 s | 0x1D1C8000 | **77%** (RSTAB **100% SCENE**) | **Full-screen blue orb + horizontal energy-wave loading/intro scene** (the RB3 "warm-up" graphic) — real render, real progress past the splash |
| 0500 | 13.0 s | 0x1D1C8000 | 77% (SCENE) | same blue-orb loading scene |
| 0600 | 15.5 s | **0x1CE30000** | **0%** | **BLACK** — frontbuffer flips to 0x1CE30000, read fresh (`decision=UPLOAD`), genuinely 0% (game drew nothing) |
| 0700 | 18.0 s | 0x1CE30000 | 0% | **BLACK** — `SKIP_STALE`, no new draws; ~19 more raw swaps, then the title exits |

**Non-zero pixel % does NOT rise again after the black window.** The single
content-bearing screen this build ever reaches is the blue-orb **loading** scene
(frames ~400–500); `main_hub` / an intro movie / gameplay never appear.

During boot the game streams **all 10 content archives** (`main_xbox_0.ark …
main_xbox_9.ark`) — consistent with the blue orb being a *loading* screen — and
then goes idle (zero `NtCreateFile`/`NtReadFile` in the black window). No runtime
`XamShow*`/`XamContent*` call; users 0 and 1 sign in cleanly. So no blocking
system UI and no crash gate the exit.

### How this refines L2/L3/L4

- L3's "DRAWS-STOPPED after frame 500 / real black" is **correct** — but the
  draws stop because the App is **entering its exit/shutdown path**, not parking
  forever. The black is the App's cleared buffer as it winds down.
- L2's "main thread parks in the present/vsync wait" holds **only up to ~18 s**;
  then tid6 leaves the loop and runs guest shutdown (`NtSetEvent`) → title exit.
- L4's content/save/movie refutations still stand (no runtime XAM calls, no Bink).
  The exit is not a *blocking* content dialog — it is the App **deciding to quit**.

### Root-cause hypothesis + handoff

A **guest-side early title exit after content load.** Most consistent with the
documented clean-TU5 blocker (`docs/rb3-bringup-notes.md`): *"reaching the RB3
overshell on clean TU5 is blocked by a game-side content check … needs matching
genuine TU5 title-update content."* After streaming the ARKs and initializing,
RB3 most likely fails a content / save / title-update integrity check and bails
(exit-to-dashboard) instead of proceeding to `main_hub`. **A longer timeout is
irrelevant; the fix must satisfy or bypass that guest check.**

Next steps (L1):
1. Trace `tid=6` guest code in the 15–20 s window to catch the exact call that
   initiates shutdown (the flow node that decides to quit). Park site is
   `guest_lr=0x82844CF8` (present/vsync wait); the decision fires just before
   tid6 stops parking and issues `NtSetEvent handle=0xF80000EC`.
2. Boot with **genuine TU5 title-update content** mounted and check whether the
   ~20 s exit disappears / `main_hub` renders.
3. Cross-check **DC3** on this fork runs to the full timeout (does *not* self-exit
   at ~20 s) — confirms the ~20 s exit is RB3-content-specific, not an
   emulator-wide teardown bug.

---

## DECISION (synthesis of L1–L4, 2026-07-08)

**Single highest-confidence blocker: a GUEST-SIDE content / title-update
integrity gate in RB3 clean TU5 that fails (no matching genuine TU5 `update:`
package is mounted), so the App stops issuing draws (~frame 500 / ~13 s), keeps
presenting the last buffers for a few seconds, then tid6 runs its own orderly
teardown and the title exits at ~18 s — deterministically, timeout-independently,
and crash-free — before `main_hub` ever renders.** This is the same documented
dirty-disc-family blocker (see Session 3 in `docs/rb3-bringup-notes.md`); the
`clean_tu5_nodd.xex` patch NOP'd only the *earliest* bail (letting boot reach the
blue-orb load scene), but a *downstream* integrity/flow gate still fails and the
App shuts itself down.

### Why this, ranked by decisiveness of evidence
- **L1 control experiment (STRONGEST):** identical teardown at **18015 ms** with
  `--headless_timeout_ms=600000` vs 18014 ms at 120000 → the exit is **not** the
  headless timeout. Guest-initiated: `NtSetEvent tid=6 handle=0xF80000EC`
  (l1-longrun.log:5069) precedes an orderly handle-teardown cascade. This
  **overturns the L2/L3/L4 premise** that the run is "killed by the 120 s
  timeout."
- **L3 (decisive on capture):** `FlushDeferredDraws` count frozen at 5 after
  frame 500; frame 600 reads the *new* frontbuffer `0x1CE30000` **fresh**
  (`decision=UPLOAD`) and still 0 % non-zero → the black is real, **not** a
  capture bug. **Rejects L4's `command_processor.cc` handoff.**
- **L4 (decisive on content/dialog/movie):** 0 runtime `XamShow*` / `XamContent*`
  / movie / save-enum calls (the 82000xxx lines are the IAT import listing).
  **Rejects** blocking-dialog, save-stall, and intro-movie hypotheses.
- **New verification (this synthesis):** the ~18 s exit does **not** invoke
  `XamLoaderLaunchTitle` / `XamLoaderTerminateTitle` / `XamShowDirtyDiscErrorUI`
  at runtime (grep of l1-longrun.log: only the import table at lines 477/488/495).
  So the exit is the **App's own Shutdown()**, consistent with a content-integrity
  flow gate deciding to quit rather than a dashboard-exit import.

### Fix spec (priority order; both in-our-control + DC3-safe)

**PRIMARY — diagnose-then-patch the second gate → `clean_tu5_nodd2.xex`:**
1. Add a **cvar-gated diagnostic** (`--rb3_trace_shutdown`, default OFF → inert
   for DC3) in `src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc` `NtSetEvent`:
   when called from the main/App thread after draws have stopped, log the guest
   **LR + a shallow guest-stack return-address walk**. The caller of the last
   `SetEvent(0xF80000EC)` before teardown = the shutdown decision site.
2. Disassemble that caller in **rb3-xenon**: flat image
   `/home/free/code/milohax/rb3-xenon/_tu5probe/clean/band_clean_tu5.exe`
   (`file_off = VA - 0x82000000`), or `bin/analyze-function` via
   `ghidra.local:8001`. Expect it near the dirty-disc helper family
   (`0x8283D740 ShowDirtyDiscAndBail`, `0x8283D4F0 exit-to-dashboard`). Find the
   conditional branch gating the exit.
3. Patch that branch in the XEX (same tooling that produced `_nodd`) to always
   continue → `clean_tu5_nodd2.xex`. Expect draws to resume and the frontbuffer
   to advance to `main_hub`. **For the same-instrument proof, apply the same
   second-gate NOP to `clean_tu5_nodd_siPATCH.xex`** (so the cave detours are
   present when the overshell is finally reached).

**SECONDARY (faithful confirmation) — mount genuine TU5 `update:` content:**
source a *matching* TU5 title-update (real `patch_xbox.hdr/.ark`, not the `LOLZ`
placeholder) and mount at `update:`. If the ~18 s exit disappears on the
**unpatched** `clean_tu5`, that confirms the content-integrity root cause and
validates PRIMARY isn't masking a different problem.

**CONTROL — DC3 cross-check:** boot DC3 to the full timeout; confirm it does NOT
self-exit at ~18 s → proves the exit is RB3-content-specific, not an emulator
teardown bug.

**EXPLICITLY REJECTED:** any `command_processor.cc`/capture change (L3);
kernel XAM/content stub or harness dialog-dismiss (L4 — the exit doesn't route
through the dirty-disc import); a longer timeout (L1); an emulator "block title
exit" hack (DC3-unsafe, papers over the real gate).

### Boot command (after producing the nodd2 build)
```bash
# stage: cp clean_tu5_nodd2.xex /tmp/rb3_nodd/default.xex   (or siPATCH+nodd2 for the A/B)
build/bin/Linux/Checked/xenia-headless \
  --target=/tmp/rb3_nodd/default.xex \
  --protect_zero=false --local_user_count=2 --gpu=vulkan \
  --dump_frames_path=/tmp/rb3menu-wf/nav-frames --headless_capture_interval=100 \
  --headless_timeout_ms=120000 2>&1 | tee /tmp/rb3menu-wf/nav-run.log
```
Success = draws no longer freeze at `flush#5`; a non-black `main_hub` renders
(`nonzero_pct>15` SCENE with menu geometry), and the title does NOT tear down at
~18 s.

### 2-controller nav plan (once main_hub renders)
Time-based `--scripted_input`, pad suffix `@0`/`@1`. Path (RB3 Wii decomp):
`main_hub` (both join overshell) → Quickplay → `song_select` → pick song →
`part_difficulty` (both pick **Guitar**) → gameplay. Start from
`rb3-verify/scripts/two_guitar_p1p2.txt`, calibrate timings against one boot's
frame log (main_hub now lands ~13–15 s):
```
16s:A@0, 20s:START@0, 26s:A@1, 30s:A@0, 36s:A@0, 42s:A@0, 46s:A@1, 52s:A@1, 56s:A@0
# A@0 dismiss splash→hub; START/A@0 Quickplay; A@1 P2 joins overshell;
# A@0 into song_select + pick song; A@0 P1 Guitar; A@1 P2 Guitar (same instr);
# A@1/A@0 confirm difficulty → gameplay.
```
Capture frames each step; proof = a gameplay frame with two Guitar tracks + both
players active (exercises the same-instrument cave detours on the siPATCH build).

---

## IMPLEMENTER PASS (2026-07-08): diagnostics shipped, root cause refined, decision.json premise partly overturned

Commit `292ea0c18` (branch `headless-vulkan-linux`). Two DC3-safe, default-OFF
diagnostic cvars added; the shutdown was traced and disassembled. **main_hub did
NOT render** — but the blocker is now characterized far more precisely, and the
"dirty-disc-family / NOP-a-branch" fix template from `decision.json` is shown not
to apply as written.

### What shipped
- **`--rb3_trace_shutdown`** (`src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc`,
  in `xeNtSetEvent`): logs, the first time each distinct **tid-6 NtSetEvent
  caller chain** appears, the guest LR + a shallow guest-stack return-address
  walk. Keys on the **call-site chain**, not the raw handle — kernel handle
  numbers are **non-deterministic across runs** (the same `0xF80000EC` is an
  `XEvent` in one boot and an `XMutant` in the next; keying on the handle value
  as originally spec'd is unreliable).
- **`--rb3_mount_update`** (`src/xenia/emulator.cc`): registers `update:` → disc
  dir so RB3 finds `update:\gen\patch_xbox.hdr`.

### Findings (diagnostic run: `default.xex` = `clean_tu5_nodd.xex`, `--rb3_trace_shutdown`)
1. **The NtSetEvent shutdown trigger is teardown EXECUTION, not the decision.**
   The last tid-6 `NtSetEvent` before the handle-teardown cascade (NEW-CHAIN #88,
   `diag-run3.log:6399`) has caller chain `82C512CC → … → 82744BCC/82744D28 →
   823FEC1C → 8275CC78 → …→ App-loop thunks 82404988/82404DF4`. Disassembling
   every frame (flat image `band_clean_tu5.exe`, `file_off = VA − 0x82000000`)
   shows they are the App's **destructor/teardown cascade**: virtual dispatch
   (`bctrl`), RTTI base-offset resolution (`lwz r11,4(r3); lwz r11,4(r11); add`),
   `__savegprlr`/`__restgprlr` thunks. The event signal here just wakes a worker
   during teardown. **The decision to quit is UPSTREAM, in the App::Run flow, and
   is not present in any NtSetEvent stack** (the normal-loop chains resolve to
   inlined singleton-getters/allocators — `mulli …,0xaa` hashmap, `clrlwi.;ori 1`
   one-time-init — also not the decision).
2. **The exit is a clean `App::Shutdown()`, NOT a dirty-disc bail.**
   `XamShowDirtyDiscErrorUI` **does not fire at runtime** (the only occurrence is
   the IAT import listing `82000468 82C4BDEC`; the `xam_ui.cc` guest-caller logger
   stays silent). This **overturns the `decision.json` "same dirty-disc-family
   blocker" framing** and its fix step "NOP the gating conditional branch near
   `0x8283D740`/`0x8283D4F0`" — this second gate does not route through the
   dirty-disc helpers at all. The `_nodd` mflr→blr patch template (which
   neutralised the *first*, pre-frame dirty-disc bail at a function turned into an
   immediate `blr`; XEX off `0x519320`, one instr `7D8802A6`→`4E800020`) has **no
   equivalent single branch located** for the second gate.
3. **The only file failure in the entire boot is `update:\gen\patch_xbox.hdr`**
   (`0xC000000F`, `update:` intentionally unmounted). All 10 `main_xbox_*.ark` +
   `main_xbox.hdr` + `charnames.zbm` load cleanly (`charnames.zbm` = full 258-byte
   EOF, **not** a truncation). So the gate is **not** a bad content read.
4. **SECONDARY lever (genuine TU5 update) is dead.** The only `patch_xbox.hdr`
   available (`/srv/torrents/games/arbys/rb3/gen/`, and the RB3DX copy) has
   **`LOLZ` magic** (`4c4f4c5a`), the placeholder — vs the base `main_xbox.hdr`'s
   genuine encrypted `E8036F60…`. Booting `_nodd` with `--rb3_mount_update=true`
   makes patch_xbox open but the title exits **earlier** (469 vs ~700 swaps, ~9 s
   vs ~18 s), no dirty-disc — confirming the placeholder is rejected. A genuine
   matching TU5 title-update package is required and is not on disk.

### DC3-safety (control)
Both cvars default OFF and change no code path for DC3 (`rb3_trace_shutdown` = log
only; `rb3_mount_update` = one extra symlink DC3 never probes). Verified: DC3
boots with the new binary, neither cvar fires, and it runs to 30 s+ with **no
~18 s self-exit** (the ~18 s self-exit is RB3-content-specific).

### New frontier (for the next pass)
The blocker is a **guest-side App::Run flow decision to quit after the loading
scene completes**, upstream of the teardown, and **not** a dirty-disc branch.
The next diagnostic must catch the **App::Run loop-break** directly (instrument
where tid-6 stops issuing `VdSwap`/stops re-parking at the frame-wait
`guest_lr=0x82844CF8` and begins the destructor cascade), or trace the boot-flow
state machine that transitions loading → (shell/main_hub vs shutdown). The NtSet
Event angle is exhausted. Candidate mechanism: the post-warmup shell load
requires TU5 patch content that is absent/placeholder, so the flow has no valid
next state and returns — but the exact validation/branch is not yet located.
Artifacts: `/tmp/rb3menu-wf/diag-run3.log` (trace), `/tmp/rb3menu-wf/upd-run.log`
(update-mount experiment), `/tmp/rb3menu-wf/disas.py` (flat-image disassembler).
