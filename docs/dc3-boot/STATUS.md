# Status: OODA Loop Iteration 2

*Last updated: 2026-02-23 (Sessions 12-13 — Game runs stably, NUI callback loop blocker)*

## 2026-02-23 Update (Sessions 12-13): JIT Indirect Call Fix + Game Stability

### Major Breakthrough: Game Runs Without Crashing

The game now runs for the full 30-second timeout (exit code 124) with **zero SIGSEGV crashes**.
4.4MB of log output. The critical path advanced from "CRT init crash" to "game main loop running
but stuck in NUI callback dispatch."

### Fixes Applied (x64_emitter.cc, x64_code_cache.cc, x64_code_cache.h)

1. **CallIndirect bounds check**: JIT indirect calls (`bctrl`) now check if the guest address
   is within the indirection table range `[0x80000000, 0x9FFFFFFF]`. Out-of-range addresses
   (like XAM COM vtable entries at `0x40002830`) fall through to the resolve function thunk
   instead of reading unmapped memory.

2. **Resolve function thunk for out-of-range calls**: The slow path uses the resolve function
   thunk (which properly saves/restores volatile JIT registers) instead of calling ResolveFunction
   as a C function. This prevents clobbering rsi (context), rdi (membase), r10, r11, xmm4-15.

3. **No-op return stub for unresolvable functions**: When `ResolveFunction` can't find a function
   (null target, XAM COM entries, garbage pointers), it returns a single `ret` instruction placed
   in the code cache. This lets the game continue instead of crashing.

4. **Indirection table bounds checks**: `AddIndirection` and `PlaceGuestCode` in x64_code_cache.cc
   now skip indirection table updates for guest addresses outside the valid range.

5. **Register ordering fix (Linux SysV ABI)**: Fixed the old-style resolve path to read `rsi`
   (context) into `rdi` BEFORE overwriting `esi` with the target address.

6. **Made indirection table constants public**: `kIndirectionTableBase` and `kIndirectionTableSize`
   moved from `protected` to `public` in x64_code_cache.h for use in bounds checks.

### Current Blocker: NUI Callback Dispatch Loop

The game is stuck in a tight loop at guest `0x834B1F40` iterating a linked list of callback
function pointers. The function pointers are garbage:
- `0x38600000` (PPC instruction `li r3, 0` — not a valid function address)
- `0x00000000` (null)

These are handled gracefully by the no-op stub (~29,754 calls each in 30 seconds), but the loop
never terminates. The callback list at `[r30+0x20]` (object at `0x83C16B40`) is part of the NUI
subsystem and was never properly initialized because NUI was stubbed.

```
0x834B1F40: lwz r11, 0x0008(r29)   ; load function pointer from list node
0x834B1F44: mtctr r11               ; CTR = function pointer
0x834B1F48: bctrl                   ; call → no-op stub returns
0x834B1F4C: lwz r29, 0x0000(r29)   ; r29 = next node in linked list
0x834B1F50: cmplw cr6, r29, r30    ; compare with end sentinel
0x834B1F54: bne- cr6, 0x834B1F40   ; loop if not done → infinite
```

### Documentation Created

- `docs/dc3-boot/DEBUGGING_TIPS.md` — comprehensive debugging guide covering JIT architecture,
  memory map, indirection table, volatile register dangers, crash patterns, and diagnostic
  instrumentation. Captures hard-won knowledge from sessions 10-13.

## 2026-02-23 Update (Guest Override / Patch Hardening In Progress)

- Added CPU/backend support for **guest function extern overrides on normal direct guest calls** (`bl` path), not just import-thunk externs.
- Added `Processor` guest override registry (`Register/ClearGuestFunctionOverride*`) and extern binding during guest function lookup.
- DC3 NUI/XBC stub path can now use **guest extern overrides** for eligible simple `li r3, {0|-1}; blr` stubs (cvar-gated), with byte-patch fallback for unsupported/unresolved entries.
- Preserved fake Kinect skeleton injection behavior by not overriding `NuiSkeletonGetNextFrame` when `fake_kinect_data` is enabled.
- Added safer byte-patch behavior:
  - skips zero-filled targets
  - skips unmapped/translate-failed targets
  - validates target is inside `.text` when section metadata is available
- Added `.text` FNV-1a fingerprint logging and `dc3_nui_patch_layout=auto|original|decomp` with optional fingerprint-based auto matching cvars (`dc3_nui_layout_fingerprint_{original,decomp}`).
- Added optional fingerprint cache file support (`docs/dc3-boot/dc3_nui_fingerprints.txt`) with auto-probe in Xenia.
- Added a first-pass **resolver pipeline** for DC3 NUI/XBC patch targets:
  - patch manifest JSON (`xenia_dc3_patch_manifest.json`) -> symbol manifest (`symbols.txt`-style `.text` entries) -> signature hook (stub) -> catalog fallback
  - `dc3_nui_patch_resolver_mode=hybrid|strict` (legacy mode removed later in the cutover)
  - per-entry resolver-method logging in patch/override output
- Added `dc3-decomp` post-link manifest emitter (`scripts/build/generate_xenia_dc3_patch_manifest.py`) and `build_xex.py` integration (best-effort generation).
- Fingerprint note: the decomp `.text` fingerprint must be computed from the **final packed XEX** (after thunk-marker rewriting), not the pre-pack `default.exe`.
- Added override cleanup on title terminate / new launch path to avoid stale mappings across runs.

This is an **implementation enabler** for retiring raw guest byte patches. Full semantic resolver work (fingerprint cache + symbol/signature resolution) is still in progress.

## Current Milestone: Past `main()`, Advancing Through Game Init

Progress: **Game runs stably with zero crashes.** CRT init completes. `main()` reached.
XAM loaded. JIT indirect calls to unresolvable functions (XAM COM vtables, null pointers)
handled gracefully with no-op stubs. Current blocker: NUI callback dispatch infinite loop.

## OBSERVE — What happens when we run the test?

### What works
- XEX loads, 323 thunks + 501 variable imports resolved (100%)
- 62/62 NUI (Kinect SDK) functions stubbed in guest memory
- XapiCallThreadNotifyRoutines stubbed (CRT list corruption bypass)
- Main thread stack increased 256KB -> 1MB
- RODATA pages (0x82000000-0x82410000) made writable for /FORCE unresolved globals
- CRT sanitizer: **364/390 constructors execute** (26 nullified -- all point to 0x82000000)
- Main XThread starts, VdSwap imported
- RtlEnterCS=186, RtlInitCS=10, RtlLeaveCS=185 before crash
- MAP address extractor validated: 70/70 symbols match emulator.cc (Agent E)

### Where it crashes
- **RtlpCoalesceFreeBlocks** (rtlheap.obj @ 0x830DC644) -- SIGSEGV reading guest address 0
- Heap metadata overwritten: r30=r31=0x536F7264, r25=0xFEEEFEEE
- Same pattern seen in Session 5 -- persistent, deterministic decomp-side bug
- Crash occurs during CRT static initialization (before game `main()`)

### Wave 1 key finding: "Sord" is NOT a string literal (Agent B)
- Zero matches for "Sord" in any .obj file or MAP symbol
- 0x536F7264 is **corrupted heap free-list pointer** data, not string data
- The ASCII interpretation as "Sord" is coincidental
- 0xFEEEFEEE = MSVC debug fill for freed memory → **use-after-free pattern**

### Linker error landscape (dc3-decomp) — Updated counts (Agent D)
| Category | Count | Notes |
|----------|-------|-------|
| Unique unresolved symbols (total) | **867** | 334 non-string + 533 string literals |
| `lbl_` assembly labels | 195 | Split boundary gaps |
| `??__E` CRT constructors | 26 | **CRITICAL** -- nullified, leave globals uninitialized |
| SEH handlers (`__except_handler*`) | 48 | Low runtime impact |
| `merged_` symbols | 19 | Compiler-merged data |
| Third-party (libjpeg, curl, ogg, zlib) | 18 | Need stub .c files |
| String literals `??_C@` | 533 | COMDAT extraction broken in jeff |
| LNK4006 (duplicate symbol) | 411 | Harmless with /FORCE |
| LNK2013 (REL14 overflow) | 3 | Branch distance |

### COMDAT string failure root cause (Agent C)
Two-part failure:
1. **Jeff commit `cf01a80`** restricted COMDAT extraction to code-only → `.rdata` strings
   no longer get `IMAGE_COMDAT_SELECT_ANY` wrapping → linker can't deduplicate them
2. **Hash mismatch**: decomp compiler uses `A` hash for `??_C@` symbols, original binary
   uses full CRC hash (e.g., `JPIJPPAL`) → same string gets different symbol names
Fix: hash normalization in jeff's xex.rs (~15 lines of Rust)

### 26 nullified CRT constructors — what they leave uninitialized (Agent D)
| Symbol | Global | Risk |
|--------|--------|------|
| `??__ETheMemcardMgr@@YAXXZ` | TheMemcardMgr | **HIGH** -- memory allocation |
| `??__EThePresenceMgr@@YAXXZ` | ThePresenceMgr | **HIGH** -- network state |
| `??__ETheVirtualKeyboard@@YAXXZ` | TheVirtualKeyboard | **HIGH** -- UI state |
| `??__E?gThreadAchievements@@...` | Achievements::gThreadAchievements | **HIGH** -- vector |
| `??__E?sRoot@FilePath@@...` | FilePath::sRoot | MEDIUM -- path state |
| `??__E?sNull@FilePath@@...` | FilePath::sNull | MEDIUM -- path state |
| `??__EgDataPointMgr@@YAXXZ` | DataPointMgr::gDataPointMgr | MEDIUM |
| `??__EgWavMgr@@YAXXZ` | WavMgr::gWavMgr | MEDIUM |
| `??__EgChecksumData@@YAXXZ` | FileChecksum::gChecksumData | LOW |
| `??__EgEntries@@YAXXZ` | MessageTimer::gEntries | LOW |
| `??__EgOverride@@YAXXZ` | Keyboard::gOverride | LOW |
| 15x `??__Ek*ChunkID@@YAXXZ` | kListChunkID, kRiffChunkID, etc. | LOW -- 4-byte int constants |

These constructors exist in the asm .obj files at known MAP addresses (0x82A2E81C-0x82EE9D30)
but the decomp .obj replacements don't provide them → linker resolves to 0x82000000 → nullified
by CRT sanitizer → globals remain zero-initialized.

### ~110 NUI SDK constructors are safe to nullify (Agent B)
| Index Range | .obj Source | What |
|-------------|-----------|------|
| 98-114 | xspeechapi, facedetector | NUI speech + face detection (17) |
| 127 | sceneestimation | Scene estimation (1) |
| 133-165 | frontaldetector, multiposedetector | Face/pose detection (33) |
| 171-174 | speedmeasurement, Skeleton | Speed + skeleton vectors (4) |
| 272-306 | nuifitnesslib..nuiaudio | NUI fitness/face/head/audio (35) |
| 307-314 | NUISPEECH globals + COM | Speech globals + COM objects (8) |
| 319-330 | fecommon..rtcommon | COM arrays, D3D globals (12) |

These call unresolved internal NUI functions → execute PE header bytes → garbage results → heap corruption.
Safe to nullify since 62 NUI API functions are already stubbed.

### Repo states
| Repo | State | Key Issue |
|------|-------|-----------|
| **xenia** | 18+ files uncommitted on `headless-vulkan-linux` | Binary needs rebuild for bisect cvar |
| **dc3-decomp** | Working tree dirty, recent link with fixed jeff | 867 unresolved symbols |
| **jeff** | 9 files changed (BSS fix, COMDAT fix, CRT co-location) | Uncommitted |

## ORIENT — What does it mean?

### Critical path (updated)

```
XEX loads -> imports resolve -> NUI stubbed -> CRT init -> main() -> XAM load -> [NUI CALLBACK LOOP] -> App::Init -> ...
```

We are past `main()` and into the game's initialization sequence. The game is stuck in a
**NUI callback dispatch loop** — iterating a linked list of function pointers from the NUI
subsystem that were never properly initialized.

### Refined hypotheses (from Agent B + D convergence)

**Hypothesis A (CONFIRMED LIKELY): NUI SDK constructors call unresolved internal functions.**
~110 CRT constructors from NUI/Kinect SDK libraries (indices 98-330) initialize complex COM/ATL
objects, private heaps (`g_heap@NUISPEECH`), D3D globals (`g_RTGlobal`). These call internal SDK
functions NOT among the 62 stubbed NUI API functions. Those internal functions resolve to 0x82000000
(RODATA/PE header) via `/FORCE:UNRESOLVED` and "execute" as garbage PPC code, producing corrupt
return values that overwrite heap metadata.

**Hypothesis B (CONFIRMED LIKELY): 26 nullified constructors leave critical globals uninitialized.**
TheMemcardMgr, ThePresenceMgr, TheVirtualKeyboard, gThreadAchievements, FilePath::sRoot etc.
remain zero-initialized. Later CRT constructors (or `_cinit()` itself) dereference these null globals,
causing use-after-free (0xFEEEFEEE pattern) when they touch freed/invalid heap memory.

**Both hypotheses are likely active simultaneously.** NUI constructors corrupt heap metadata directly,
AND uninitialized globals cause indirect corruption when later code uses them.

### What's NOT blocking us
- NUI API stubbing: working (62/62)
- Import resolution: working (707/707)
- JIT compilation: working (arena + label fixes applied)
- CRT sanitizer: working (26/390 nullified, correct threshold)
- RODATA workaround: working (still needed)
- Xenia Linux platform: working (QueryProtect, SIGSEGV handler, MMIO all fixed)
- MAP address extraction: validated, production-ready (70/70 match)

## DECIDE — What's the highest-leverage fix?

**Priority 1 (NOW): Break the NUI callback dispatch loop.**
The game is stuck calling garbage function pointers in a linked list at guest 0x834B1F40.
Options: (a) stub the dispatch function itself to skip the loop, (b) patch the linked list
to be empty (sentinel == head), (c) add a call-count limiter for no-op stub targets.

**Priority 2: Identify what game code runs after the callback loop exits.**
Once the loop is broken, observe the next crash/hang and iterate.

**Priority 3: Fix the 26 missing CRT constructors in dc3-decomp/jeff.**
The 11 complex constructors (TheMemcardMgr, etc.) need their `??__E` symbols exported from
the asm .obj files, or the decomp source must define proper C++ initializers.

**Priority 4: Fix COMDAT string extraction in jeff.**
Hash normalization in xex.rs to resolve 533 unresolved `??_C@` string literals.

**Priority 5 (ONGOING): Reduce remaining 195 lbl_ + 48 SEH + 18 third-party symbols.**

## ACT — See TODO.md

---

### Session 11 (2026-02-23): DC3 NUI/XBC Resolver + Guest Override Validation (in progress)
- Verified local Xenia build system on this repo: `./xb build` (Make-based), not a required Ninja flow
- Built `xenia-headless` successfully after guest override + resolver changes
- Ran timed headless smoke on `/home/free/code/milohax/dc3-decomp/build/373307D9/default.xex`
- Confirmed manifest-first NUI/XBC resolver is active with `--stub_nui_functions=true`
  - `manifest_hits=63`, `symbol_hits=0`, `catalog_hits=1` (64 total NUI/XBC patch specs)
  - `Registered 64 guest extern overrides...`
  - `NUI patch/override summary: patched=0 overridden=64 skipped=0`
- Confirmed direct guest-call extern path works in practice (no byte patching needed for eligible simple stubs)
- Found `.text` fingerprint mismatch between manifest hash and Xenia runtime hash
  - Manifest currently hashes static PE `.text`
  - Xenia logs runtime `.text` bytes after module load (different hash in this build)
  - Added fallback: when an explicit manifest path is provided, trust `build_label` for layout selection and log mismatch
- Extracted DC3 NUI/XBC resolver/parsing code out of `emulator.cc` into:
  - `src/xenia/dc3_nui_patch_resolver.h`
  - `src/xenia/dc3_nui_patch_resolver.cc`
- Added manifest parser support for optional runtime fingerprint field:
  - `pe.text.xenia_runtime_fnv1a64` (preferred for layout matching when present)
- Updated dc3-decomp manifest generator to emit explicit fingerprint semantics:
  - `pe.text.fnv1a64` (artifact/static hash currently used by generator)
  - `pe.text.fnv1a64_static_pe`
  - optional `pe.text.xenia_runtime_fnv1a64`
- Added signature resolver implementation (strict/hybrid path) for:
  - Cross-build (original + decomp): `NuiInitialize`, `NuiShutdown`, `NuiMetaCpuEvent`, `CXbcImpl::{Initialize,DoWork,SendJSON}`
  - Decomp-only (currently): `D3DDevice_Nui{Initialize,Start,Stop}`
  - unique-match + validation + local hint window around catalog addresses
- Added `dc3_nui_signature_trace` cvar to dump runtime PPC words for signature-backed targets
  - Used to retune decomp signatures from real runtime patch-site bytes (not function-start symbols)
- Signature resolver hardening + current status:
  - Resolver now uses local signature hints to adjust manifest/symbol targets to actual patch sites (for known signature-backed targets)
  - Current decomp hybrid manifest run resolves all entries directly from manifest in this branch state:
    - `manifest_hits=85`, `signature_hits=0`, `catalog_hits=0` (85 total)
    - `Registered 85 guest extern overrides...`
  - Current original hybrid run (with decomp manifest still auto-probed) falls back cleanly to symbols/catalog:
    - `manifest_hits=0`, `symbol_hits=56`, `catalog_hits=3` (59 total)
    - `Registered 59 guest extern overrides...`
  - Strict/signature-only smoke (no manifest/symbols, `--dc3_nui_enable_signature_resolver=true`) now resolves:
    - decomp: `85/85` (`signature_hits=85`, `strict_rejects=0`)
    - original: `59/59` (`signature_hits=59`, `strict_rejects=0`)
  - Current strict signature-covered targets:
    - Full current DC3 NUI/XBC patch tables in this branch state:
      - original: all `59/59`
      - decomp: all `85/85` (including `Nuip*`, `D3DDevice_Nui*`, speech/fitness/wave/head, camera/identity)
  - Resolver remains fail-closed on local hint misses and ambiguous matches (no unsafe global clone fallback)
- Added `xenia-core-tests` suite with resolver unit tests (Catch2):
  - hex parser / fingerprint cache parser / manifest parser (runtime fingerprint field)
  - strict signature resolve, strict fail-closed behavior
  - strict ambiguity rejection (global multi-match) and local-miss/global-clone fail-closed behavior
  - speech-family ambiguity regression (orig+decomp `NuiSpeechEnable` variants duplicated -> reject)
  - hybrid manifest-target adjustment via local signature hint
  - synthetic coverage matrix for current signature-backed targets (original + decomp variants as supported)
  - Current filtered run: `All tests passed (648 assertions in 10 test cases)`
- Runtime-validated decomp fingerprint for Xenia layout cache: `0x28F813763596C6C6`
- NUI/XBC cutover defaults promoted in `emulator.cc`:
  - `dc3_guest_overrides=true` (default-on; legacy NUI/XBC byte-patch fallback removed)
  - `dc3_nui_enable_signature_resolver=true` (default on)
  - `dc3_nui_patch_resolver_mode` remains `hybrid` (default)
- Cutover validation matrix (2026-02-23):
  - Default path, original XEX:
    - `guest_overrides=1`, `resolver_mode=hybrid`
    - `Registered 59 guest extern overrides`
    - `patched=0 overridden=59 skipped=0 total=59`
  - Default path, decomp XEX:
    - `guest_overrides=1`, `resolver_mode=hybrid`
    - `Registered 85 guest extern overrides`
    - `patched=0 overridden=85 skipped=0 total=85`
  - Strict signatures-only (no manifest/symbols), original:
    - `signature_hits=59`, `strict_rejects=0`
    - `patched=0 overridden=59 skipped=0 total=59`
  - Strict signatures-only (no manifest/symbols), decomp:
    - `signature_hits=85`, `strict_rejects=0`
    - `patched=0 overridden=85 skipped=0 total=85`
  - Conclusion:
    - DC3 NUI/XBC is cut over to resolver+guest-override by default.
    - Follow-up validation cleared removal of the legacy DC3 NUI/XBC byte-patch path.
- Legacy-removal roadmap (DC3 NUI/XBC only; not CRT/skeleton behavior hacks, now completed):
  1. Add a cutover gate script / CI check that runs `original+decomp` in `hybrid` and `strict` and asserts `patched=0`, `unresolved=0`.
  2. Keep legacy fallback path for one validation window while collecting logs from real workflow runs. (Done)
  3. Remove DC3 NUI/XBC byte-write application for entries fully handled by guest overrides (keep non-NUI legacy hacks separate). (Done)
  4. Delete `dc3_guest_override_poc` alias and update docs/scripts to `dc3_guest_overrides`. (Done)
  5. Reassess whether `resolver_mode=strict` can become the default after manifest/symbol workflows are stable. (Open)
- Cutover gate automation added:
  - `tools/dc3_nui_cutover_gate.sh`
  - Runs original+decomp across:
    - default (`hybrid` + guest overrides)
    - strict signatures-only (manifest/symbol disabled)
  - Asserts resolver/apply summaries and `TIMEOUT` completion.
  - Current result: passed twice consecutively (deterministic) on local runs.
- Legacy DC3 NUI/XBC byte-patch application path removed after cutover validation:
  - NUI/XBC now uses a single apply path: resolver -> guest extern override registration
  - `dc3_guest_overrides=false` now warns and is forced on (legacy byte patching removed)
  - Post-removal validation:
    - `tools/dc3_nui_cutover_gate.sh` (default+strict) passed on original + decomp
    - `xenia-core-tests \"[dc3_nui_patch_resolver]\"` passed (`648 assertions / 10 test cases`)
  - Rollback path for legacy byte patching: revert commit `411064457` (checkpoint before removal) or the legacy-removal commit itself.

---

## History

### Sessions 12-13 (2026-02-23): JIT Indirect Call Fix — Game Runs Stably
- Fixed SIGSEGV at host 0x40002830: XAM COM vtable indirect call outside indirection table range
- Added bounds check in `CallIndirect` (x64_emitter.cc) with resolve thunk fallback
- Fixed volatile register clobber: slow path was calling ResolveFunction as C function from JIT
- Added no-op return stub for unresolvable functions (null target, XAM, garbage pointers)
- Added bounds checks in `AddIndirection` and `PlaceGuestCode` (x64_code_cache.cc)
- Fixed Linux SysV ABI register ordering bug in CallIndirect
- Made `kIndirectionTableBase`/`kIndirectionTableSize` public in x64_code_cache.h
- **Result**: Game runs full 30-second timeout, exit code 124, zero crashes
- 4.4MB of log output, game is executing game code
- New blocker: NUI callback dispatch loop at 0x834B1F40 (garbage function pointers)
- Created `docs/dc3-boot/DEBUGGING_TIPS.md` capturing JIT architecture knowledge

### Session 11 (2026-02-23): NUI/XBC Resolver + Guest Override Validation
- (See detailed notes above in "2026-02-23 Update" section)

### Session 10 (2026-02-22): Heap Corruption SOLVED
- Wave 1: 5 parallel research agents completed (see docs/dc3-boot/agent_*.md)
- Fixed bisect cvar (`dc3_crt_bisect_max`): changed default from 0 to -1, fixed `>0` to `>=0`
- Fixed bisect script detection: check for crash address 0x830DC644 in log, not function name
- Fixed bisect script: xenia catches SIGSEGV internally, keeps running until timeout
- **Binary search result: Constructor #69 (0x82A9C138) is first heap corruption culprit**
  - Confirmed deterministic: 3/3 repros at N=69, 0/3 at N=68
  - Constructor 69 PPC code: initializes global at 0x83B33850 by calling function at 0x8355D730
- Implemented `--dc3_crt_skip_indices` cvar for targeted constructor exclusion
- Skipping #69 alone insufficient — NUI range 98-330 also contains culprit(s)
- **Skipping #69 + NUI 98-330 eliminates heap corruption entirely**
  - Before: RtlEnterCS=186, RtlInitCS=10 → heap crash (Sord + FEEEFEEE)
  - After: RtlEnterCS=276, RtlInitCS=13 → null vtable crash (0x830E3340)
- New crash: null vtable at 0x830E3340 — lwz r9, 4(r11) where r11=0
  - Object's vtable pointer is NULL from uninitialized global
  - This is expected: 26 nullified CRT constructors leave critical globals zeroed
- MAP was overwritten by Agent D link run (11:23 timestamp ≠ XEX 05:20 timestamp)
  - Need to rebuild dc3-decomp for consistent MAP+XEX pair

### Session 9 (2026-02-22): Clean Rebuild + Address Regeneration
- Clean rebuild of dc3-decomp with fixed jeff toolchain
- LNK4006 errors dropped 3,351 -> 411, LNK2013 dropped 17 -> 3
- Removed zero page mapping (0x0-0x10000) to surface null-pointer crashes
- Regenerated ALL 62 NUI patch addresses, Xapi stubs, CRT table bounds from MAP
- Fixed arena.cc oversized chunk assertion (guest functions > 4MB)
- Fixed finalization_pass.cc label ID overflow (functions with > 65535 blocks)
- CRT constructors executing: 320 nullified -> only 26 nullified (364/390 now run)
- Crash: heap corruption in RtlpCoalesceFreeBlocks ("Sord" + 0xFEEEFEEE pattern)

### Sessions 1-8: See docs/DC3_HEADLESS_CHANGE_AUDIT_2026-02-20.md
