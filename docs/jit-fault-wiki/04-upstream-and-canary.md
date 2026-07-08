# 04 — Upstream & Canary Intelligence

See [02-address-translation.md](02-address-translation.md) for why the fault
is a guest NULL access, and [05-fork-divergence.md](05-fork-divergence.md) for
fork ancestry.

## Headline: canary's community already root-caused this fault family

[xenia-canary/game-patches](https://github.com/xenia-canary/game-patches)
ships a per-title patch for Rock Band 3 (45410914), author **Gliniak**,
named **"Skip SEH usage to prevent nullptr write"** (`is_enabled = false` by
default):

- **Retail `default.xex`** ([45410914 - Rock Band 3.patch.toml](https://github.com/xenia-canary/game-patches/blob/main/patches/45410914%20-%20Rock%20Band%203.patch.toml)):
  patches guest `0x8226045C` → `0x485B5DDC` — **exactly our retail crash PC**.
- **TU5 `default.xex`** ([45410914 - Rock Band 3 (TU5).patch.toml](https://github.com/xenia-canary/game-patches/blob/main/patches/45410914%20-%20Rock%20Band%203%20%28TU5%29.patch.toml)):
  patches guest `0x82272E90` → `0x4280D1F1` — 4 bytes past the call frame
  `0x82272E8C` our own fork traced in the TU5 fault. (Note: our *observed*
  TU5 crash PC is `0x8275026C`; the patch site is upstream in the call chain.)
  The same file carries a disabled companion patch to sideload
  `RB3Enhanced.dll` via `writable_code_segments` — this SEH patch is commonly
  paired with RB3Enhanced in the wild.

**Interpretation:** the fault is **game-side SEH** (Xbox 360 titles use
MSVC `__try/__except` with table-based unwind, `RtlRaiseException`-style
dispatch) that Xenia does not emulate; the failed exception path ends in a
null-pointer write. Canary's workaround neuters the guest instruction that
enters the SEH-using path rather than fixing the emulator. That means:

1. There is **no emulator-source fix to port** — canary/upstream never fixed
   the underlying gap.
2. A **general fix** = implementing guest exception dispatch (the "improve the
   emulator overall" option). A **pragmatic fix** = adopting/porting the
   canary guest patch (works today, per-title).

## Emulator-source comparison (no drift, no missed fix)

- Fork merge-base `c7f61342d` (2026-01-20); upstream master `95a5c3ee2`
  (2026-02-18) is only **3 commits ahead, all GPU** — upstream's CPU backend
  has not moved since we branched.
- `ComputeMemoryAddress`/`ComputeMemoryAddressOffset` are **byte-identical**
  between our fork and upstream master; canary rewrites the file (load-store
  fusion, `elide_e0_check`) but keeps the same "clear the top 32 bits" EA
  masking. Our fork's only edits there are milo-trace instrumentation
  (`EmitMiloMemAccess`) layered on unchanged load/store emission.
- Local history has **no fix** for this family (pickaxe on `0x100000000` hits
  only our own symptom docs).

## Compatibility tracker status

- RB3 [game-compatibility #815](https://github.com/xenia-project/game-compatibility/issues/815):
  stale since 2017, `kernel-unimplemented-feature` + `state-intro`.
- DC3 [game-compatibility #1569](https://github.com/xenia-project/game-compatibility/issues/1569):
  stale since 2020, additionally `tech-kinect-required` (DC3 is separately
  gated on Kinect — our fork already carries NUI/Kinect hack-pack work).
- xenia-canary has **no compat issue for either title** and **no DC3 game
  patch** — DC3's `0x82311A94` fault has no community workaround; our fix
  should cover it too if it's the same SEH gap.

## Follow-ups this page implies

1. Decode the canary patch instructions: original bytes vs `0x485B5DDC`
   (opcode 18 = unconditional `b`) at retail `0x8226045C`, and `0x4280D1F1`
   at TU5 `0x82272E90` — what path is being skipped? →
   [03-guest-code-analysis.md](03-guest-code-analysis.md)
2. Establish what "SEH usage" is concretely in RB3's DTA parse (likely
   `__try` around symbol-table probing, or a guarded page probe) and what
   Xenia does today when a guest exception should be raised (where does the
   NULL write come from?) → [06-root-cause.md](06-root-cause.md)
3. Check whether our fork even loads canary-style `.patch.toml` (upstream
   mainline lacks the patch system; we minted `.patch.toml`s earlier for
   RB3Enhanced — verify the loader exists in-tree or port it).

## E2: canary's guest patch does NOT transfer to our TU5 boot

Follow-up (2) was executed as **Experiment E2** (see
[01-symptom-and-evidence.md](01-symptom-and-evidence.md) §"Experiment E2").
Applying canary's TU5 "Skip SEH usage" patch as a direct byte edit to
`clean_tu5_nodd.xex` (VA `0x82272E90`: `bl 0x822703d0` → canary's `0x4280D1F1`)
leaves the boot crash **byte-identically unchanged** (`crash_guest=0x8275026C`,
first fault at 3001ms, SIGSEGV=7). Adding the retail-style store-skip inside the
SEH-install routine (`0x822703FC: stw r10,0(0)` → `b 0x82270400`) on top of it
**also has zero effect**.

**Why the pragmatic option #2 from this page fails here:** canary's patch
neuters the SEH-install path (second sibling call `0x82272E90` / its internal
literal-zero write), but our **observed** TU5 crash is the *first* sibling call's
data-dependent NULL map-node string read at `0x8275026C`, which fires first and
never returns to reach the patched site (call-chain analysis in
[03-guest-code-analysis.md](03-guest-code-analysis.md)). Canary's community
patch is real and correct **for the SEH-write fault**, but our TU5 boot dies on
a *different* member of the same `0x100000000` family that the guest patch does
not cover.

**Consequence for the fix decision:** "adopt/port the canary guest patch" is
**not** a viable pragmatic unblock for our TU5 boot — it is a per-title patch
for a fault instance we don't hit first. The durable fix must target the NULL
**page-0 access** itself (E1's `protect_zero`-off proves a single zero-page-
accessibility knob clears *both* mechanisms — see
[01-symptom-and-evidence.md](01-symptom-and-evidence.md) §E1 and
[07-fix-and-verification.md](07-fix-and-verification.md) options A/B), not the
guest SEH-install instructions.
