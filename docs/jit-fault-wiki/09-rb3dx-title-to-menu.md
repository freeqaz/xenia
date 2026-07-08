# 09 — RB3 Deluxe: Title Screen to Main Hub (headless Xenia)

Phase goal: RB3DX now boots on the fixed fork (`--protect_zero=false`), renders the
ESRB/photosensitivity splashes and the animated **Deluxe title screen**, then WEDGES
in a hot recovered-fault loop before `main_hub`. Get it past the title to a
navigable menu, then drive two controllers to instrument-select.

Cross-links: [08 boot-to-menu (clean TU5)](08-boot-to-menu.md) ·
[07 fix & verification (zero-page)](07-fix-and-verification.md) ·
[02 address translation](02-address-translation.md)

---

## L2 — Fault-loop mechanism (2026-07-08)

**Fault:** host `0x1FFFFFFFC` = `virtual_membase_(0x100000000) + 0xFFFFFFFC` = guest
EA **`0xFFFFFFFC`** (= -4, top of address space). The physical 4k-page heap tops at
**`0xFFD00000`** (created `0xE0000000 + size 0x1FD00000`, memory.cc:185), so guest
`0xFFFFFFFC` is **above every heap — permanently unmapped, never legitimate guest
memory.** Host RIP is **constant** `0xA00C8B67` and guest PC **constant**
`0x827BCBD8` across every status report → the guest spins on one basic block.

### The recovery path (file:line)

`MMIOHandler::ExceptionCallback` — **`src/xenia/cpu/mmio_handler.cc:518-533`** (the
"read soft-fault"), lock acquired at `:437`.

Why this handler: `memory_->Initialize()` (emulator.cc:1829) installs
`MMIOHandler::ExceptionCallbackThunk` **before** `processor_->Setup(backend)`
(emulator.cc:1856) installs the X64Backend handler, so MMIOHandler is
`handlers_[0]` and gets first dibs (exception_handler_posix.cc:180). X64Backend's
handler only services `ud2` illegal-instructions (`0x0F0B`) and returns `false` for
access violations (x64_backend.cc:`ExceptionCallback`); the Emulator "real crash"
handler is never reached.

Flow for our fault: AV read → not an MMIO range → `QueryProtect` **under the
`global_critical_region_` lock (`:437`)** → `Memory::AccessViolationCallback`
returns false (no GPU watch on that page) → not the DC3 write-workaround (it's a
read) → not a cache hint → **read soft-fault (`:521-533`)**: `TryDecodeLoadStore`
succeeds, the dest register is zeroed (`:528`), and `ex->set_resume_pc(rip+len)`
(`:530`) advances the **host** RIP past the faulting x86 load; returns `true`.

`set_resume_pc` writes `thread_context_->rip` (exception_handler.h:157), which the
posix handler copies back into `mcontext REG_RIP` (exception_handler_posix.cc:185),
so the host *does* make progress each fault. **But the guest re-executes the same
load every loop iteration** → re-faults. Constant host RIP + constant guest PC =
tight guest spin the soft-fault cannot break.

The SIGSEGV counter increments at exception_handler_posix.cc:164
(`sigsegv_count_.fetch_add`) and is printed by emulator_headless.cc:1037-1040.

### Rate & timeline (cause vs symptom)

Fault rate is **rock-steady ~11,640/sec** (34,900 per 3 s window) from onset —
NOT escalating → a single stable spinning thread, ~85 µs/fault (signal round-trip +
global lock + `QueryProtect` syscall + decode).

Timeline (`/tmp/rb3dxboot/boot2.log`):
- `12010ms`: `SIGSEGV=0`, 26 threads.
- Last `VdSwap` = **line 5821**, right at the 15 s report boundary, **immediately
  after** `xeXamContentCreate root='rbdxcache'` (line 5799) and
  `NtCreateFile rbdxcache:\rbdxcache` (line 5816).
- `15013ms`: `SIGSEGV=11613` (onset ≈14 s), 24 threads (2 gone).
- No `VdSwap` after 5821.

**VdSwap issuance stops exactly when the fault loop begins.** The guest title
thread that presents frames entered the spin, so:

**Verdict:** the fault loop is a **SYMPTOM** of a guest-side bad pointer
(`0xFFFFFFFC`) produced right after the RB3-**Deluxe** `rbdxcache` /
`XamContentCreate` path (Deluxe-specific — L1/L3 lane), **but it is the CAUSE of
the render stall** (the presenting thread is the one spinning). The read
soft-fault handler is the **amplifier**: it converts a would-be-fatal crash into a
silent infinite wedge *and* thrashes `global_critical_region_` ~11,640×/sec, which
can starve the GPU/memory threads.

### DC3-safe fixes (ranked)

DC3 **depends on** this soft-fault path (comment `:519-520` "keeps decomp/stub
guests alive"; dc3_hack_pack_skeleton.cc:328 references a soft-faulted
`last_fault=0x100000000` guest-null read). So it **cannot be removed** — fixes must
preserve one-shot soft-faults while killing the pathological spin.

1. **(BEST) Livelock circuit-breaker** in the read soft-fault path. Track per-thread
   `(rip, fault_addr)` repetition; after N identical *consecutive* soft-faults with
   no forward progress (e.g. 4096) log once with guest PC + EA and **return false →
   real crash**. DC3's legit soft-faults advance and never repeat the same RIP
   endlessly, so it never trips for DC3. Turns the silent wedge into an actionable
   crash that unblocks L1/L3. Gate behind a cvar (default on, e.g.
   `--soft_fault_spin_limit=4096`, `0` disables).
2. **(NARROW) Refuse to soft-fault reads with guest EA ≥ `0xFFD00000`** (above every
   heap top). Such an address is unambiguously a bad pointer, never legit guest
   memory, so returning false → fatal is DC3-safe and gives an immediate diagnosis.
3. **(SECONDARY) Cut lock contention:** skip acquiring `global_critical_region_` for
   pure read soft-faults of EAs that resolve to no heap. Ameliorates GPU/render
   starvation; does **not** by itself stop the wedge.
4. **(ROOT — L1/L3 lane)** Fix the `XamContentCreate` / `rbdxcache` stub so the
   Deluxe patch isn't handed a `-4` pointer. Fixes (1)/(2) exist to surface this
   crash so it can be root-caused.

Checkpoint: `/tmp/rb3dx-wf/l2-faultloop.json`.

---

## L4 — Navigation + same-instrument patch feasibility (2026-07-08)

Prepared so the drive is ready the moment L1–L3 unblock the title-wedge fault.

### Title → main_hub input: it is **START**, not A

Ground truth from the RB3 Wii decomp's native harness (same UIManager flow as the
360 build):

- `rb3/scripts/native/song-select-capture.py` and `song-end-test.py` both open nav
  with **`@10:start, @30:confirm, …`** — the *first* post-boot input is `start`.
- `rb3/native/src/rb3_game_input.cpp`: verb `"start"` → `kPad_Start`/`kAction_Start`;
  verb `"confirm"` → `kAction_Confirm` (A button on a 360 pad).

So the RB3 "PRESS START" attract/title advances on **START**; menu selections after
are **A**. **A-only spam cannot leave press-start.**

**Was A wrong, or did the fault wedge it regardless? — BOTH.**
`rb3-verify/scripts/advance_p0.txt` already *alternated* A/START, so START was sent;
it still didn't advance because (per L2) the recovered fault **stalls frame advance
at ~frame 520** — once `VdSwap` stops, no scripted input is consumed. Independently,
pure-`A@0` attempts were also wrong for the first transition. **The nav script is
correct-by-construction but cannot be validated until the L1–L3 fault is fixed.**

### Calibrated nav script

File: `rb3-verify/scripts/rb3dx_title_to_guitar.txt` (time-based `--scripted_input`,
seconds, `@pad`; headless ≈3.6 fps → wide spacing; **times assume the fault is
fixed** and must be re-tuned on a non-wedged boot).

```
# 1 splashes (ESRB, photosensitivity):  8s:A@0,14s:A@0,20s:A@0
# 2 title->main_hub (START!):           28s:START@0,34s:START@0,40s:A@0
# 3 P2 joins at overshell:              48s:START@1,54s:A@1,60s:A@1
# 4 P1 hub->PLAY NOW->QUICKPLAY->ss:    68s:A@0,76s:A@0,84s:A@0
# 5 pick highlighted song:              94s:A@0
# 6 BOTH pick GUITAR (first slot):      104s:A@0,110s:A@1  (+UP nudges fallback)
```

Mirrors decomp path `main_hub(join) → PLAY NOW → QUICKPLAY → song_select →
part_difficulty`. **Milestone 1 (stock RB3DX):** reach instrument-select, show P2
*cannot* pick P1's instrument. **Milestone 2:** siPATCH A/B → both pick GUITAR.

### Same-instrument patch — hook addresses VERIFIED in decompressed RB3DX

`rb3-verify/patch/apply_same_instrument_clean_tu5.py` applies a 675-entry write list
(`…/tu5-migrate/orig/45410914/default_tu5_patched.writes.json`: **671 cave dwords +
4 detours**, plus `gSameInstrumentEnabled=1`). Verified byte-identical, RB3DX PE
(`band_tu5.exe`) vs clean-TU5 PE (`band_clean_tu5.exe`), imgbase `0x82000000`:

| Site | VA | RB3DX | clean-TU5 |
|---|---|---|---|
| Detour A OvershellPartSelectProvider::IsActive | `0x826684C0` | `7D8802A6` | `7D8802A6` |
| Detour B OvershellPanel::ResolvePartWaitStates | `0x825B6488` | `7D8802A6` | `7D8802A6` |
| Detour C PlayerTrackConfigList::ProcessConfig | `0x8276FA08` | `7D8802A6` | `7D8802A6` |
| Detour center TrackWatcherImpl::RecalcGemList | `0x82794740` | `7D8802A6` | `7D8802A6` |
| Code cave (2800 B, `.data` RW) | `0x82C8A000` | all-zero | all-zero |
| gSameInstrumentEnabled flag | `0x82C8AAA0` | `0x0`→`1` | `0x0` |

All four detour sites are the `mflr r12` prologue word `0x7D8802A6` (patch replaces
it with a branch to the cave). Confirms the divergence doc's **"one patch serves
both"** (`rb3-xenon/docs/plans/clean-tu5-vs-rb3dx-divergence.md`). **No address
changes for RB3DX.**

**Why the .xex can't be byte-patched directly:** RB3DX `default.xex`
(`/srv/…/rb3/default.xex`, 13.97 MB) is a **compressed+encrypted retail XEX2** — file
offsets ≠ PE image, so the flat-offset formula (`0x3000 + VA-0x82000000`) used on the
*uncompressed* clean-TU5 XEX does not apply. dtk `xex extract` yields only the PE,
not a bootable uncompressed XEX.

**Options:** (a) decompress RB3DX → bake cave into a flat image → boot the flat XEX
(hook VAs verified, but needs XEX *repack* tooling — dtk gives a PE, re-wrapping is
error-prone and can perturb the rbdxcache/LOLZ path); (b) **Xenia runtime memory
patch** generalizing the `dc3_nui` guest-memory patch loop; (c)=(a).

**Recommendation: (b) runtime memory patch — lowest risk.** After module map, gated
on `title_id==0x45410914 && version 0.0.5.1` and a new default-off cvar (e.g.
`rb3_same_instrument`), loop the 675 writes with
`xe::store_and_swap<uint32_t>(memory()->TranslateVirtual<uint8_t*>(va), new)` — the
exact mechanism the NUI block already uses in `emulator.cc` (~L2718+), which patches
guest `.text` **before the JIT compiles it** (proven timing for the 4 detours; cave +
flag are plain `.data` writes). Rationale: reuses a proven in-tree mechanism (new
code = a 675-entry table + a guarded loop); boots the **exact shipping RB3DX** we
know reaches the title (no decompress/repack, no rbdxcache/LOLZ risk); **DC3-safe by
construction** (RB3 title-id + default-off cvar → inert for DC3 `0x373307D9`); the
write list carries `old` values so the loop can assert `old==current` first.
Fallback (a) only if a dtk/xextool "emit uncompressed *XEX*" path is confirmed.

Checkpoint: `/tmp/rb3dx-wf/l4-nav.json`.

---

## L1 — DECOMPILED FAULT (real RB3DX bytes) — 2026-07-08

**Method.** clean_tu5's flat image is NOT valid for RB3DX addresses (confirmed: at
0x827BC838 clean_tu5 symbols.txt has a 0x10-byte function; RB3DX has a large one —
Deluxe code shifts everything). Decompressed the real RB3DX XEX with **xex1tool**
(built from `reverse-compiler-refs/idaxex`): the retail default.xex is already
`Not Encrypted / Not Compressed` (XEX2), so `-b` dumps the flat PE image directly.
- Image: `/tmp/rb3dx-wf/rb3dx_decomp.bin` (0xEF0000 bytes, base 0x82000000, entry
  0x8283CD20, PE name `band.exe`, filetime Sep 02 2011, TitleID 45410914 v0.0.5.1).
- Repro: `xex1tool -b /tmp/rb3dx-wf/rb3dx_decomp.bin /srv/torrents/games/arbys/rb3/default.xex`
- Disasm: capstone PPC32 BE, `off = VA − 0x82000000`.

**The faulting instruction (real bytes, corrects the clean_tu5 guess):**
```
0x827bcbd8  7f7e516e  stwux  r27, r30, r10      ; EA = r30 + r10 ; *(EA)=r27 ; r30=EA
```
It is NOT `lwz r20,-0x6ad0(r2)` (that was the clean_tu5 mis-mapping). **No r2/SDA is
involved.** EA = **r30 + r10**:
- `r30 = 0` (NULL) — loaded at 0x827bcb18 `lwz r30,0x70(r31)` = `FreeBlockInfo.mBlock`.
- `r10 = r29 << 2` (0x827bcbc0), `r29 = 0x7FFFFFFF` — loaded at 0x827bcb1c
  `lwz r29,0x7c(r31)` = `FreeBlockInfo.mPadWords`. `0x7FFFFFFF << 2 = 0xFFFFFFFC`.
- **EA = 0 + 0xFFFFFFFC = guest 0xFFFFFFFC** (= host membase+0xFFFFFFFC = 0x1FFFFFFFC,
  exactly `last_fault`). Both operands are the **FreeBlockInfo sentinel values** that
  remain when the allocator's free-block search found nothing.

**Function = `MemHeap::Alloc(int sizeWords,int align,int&allocSize)`** @ **0x827bca78**
(Milo engine, `system/utl/MemHeap.cpp`; DC3 reference matches line-for-line, with
`TryAlloc` **inlined** into `Alloc`). Base-engine code, NOT rbdxcache/Deluxe-injected.
Proof of identity:
- `lwz r11,0x1c(r3)` → `mStrategy` (MemHeap +0x1c), 4-way switch to
  FirstFit/BestFit/LRUFit/LastFit (0x827bb450/…b4c0/…b538/…b5c8).
- `FreeBlockInfo` scratch at r31+0x70: `mBlock=+0x70, mPrevBlock=+0x74,
  mSizeWords=+0x78, mPadWords=+0x7c`, init `{0,0,0x7FFFFFFF,0x7FFFFFFF}` — byte-for-byte
  the DC3 `TryAlloc` prologue.
- Failure branch (`info.mBlock==NULL`, 0x827bcb30): `FreeBlockStats` (0x827bb3e8),
  `MainThread()` (0x824a4c10), `gInsideMemFunc=false`+`gMemLock->Abandon()`
  (0x82e06e04 / 0x82e06e08 / 0x82524488), `MakeString("Allocation failure, heap
  \"%s\", want %d bytes\n   lFrags=…rFrags=…Biggest Block=…Free Bytes=…", mName,…)`
  (fmt @ 0x82117460, `mName` = `lwz r4,8(r25)` = MemHeap +0x8), `MemPrintOverview(-3,
  buf)` (**0x827bc838**, the `li r3,-3; bl` — iterates the heap table @0x82e06ba8+0x254,
  formats `[%5s] free/big/lfrag/rfrag/waste`, heap names "system"@0x82065aa4 /
  "physical"@0x82103aec), then `MILO_FAIL(buf)`, then `~String(buf)` @0x827bdf38.

**ROOT CAUSE — guest heap OOM + retail assert fall-through (NOT a Xenia reg/SDA bug).**
Because `TryAlloc` is inlined, the free-block **split** code (DC3 `if (padWords>8)
{ newBlock=(FreeBlock*)((int*)info.mBlock+padWords); newBlock->mSizeWords=…; … }`)
sits **immediately after** the failure branch with **no return/branch between them**
(failure block ends 0x827bcbb4, split starts 0x827bcbb8). On success the code reaches
the split via `bne 0x827bcbb8` (0x827bcb2c) with a valid `mBlock`; on failure it is
supposed to be unreachable because `MILO_FAIL` does not return. In **retail RB3DX the
MILO_FAIL is a no-op/returns**, so on allocation failure execution **falls through**
into the split with `mBlock=NULL` and `mPadWords=0x7FFFFFFF` →
`newBlock = NULL + 0x7FFFFFFF words = 0xFFFFFFFC` → `stwux` stores
`newBlock->mSizeWords` at **0xFFFFFFFC**. So the store address is a *deterministic
function of "this heap is out of memory"*, not of any mistranslated register.

The **infinite ~11700/s spin** = Xenia's SEH (with the `protect_zero=false` /
fb864e3e zero-page fix) catches the store at 0xFFFFFFFC, "recovers", and
**re-executes the same faulting store without advancing RIP** (resume-without-progress);
the guest is stuck in `MemHeap::Alloc`'s dead post-OOM code forever.

**Verdict on the brief's two candidates:**
- (a) Xenia r2/SDA/register-setup or mis-translated indexed load → **REFUTED.** The
  indexed `stwux` uses r30 (memory-loaded NULL) and r10 (r29<<2 sentinel), both
  produced inside this function from allocator state.
- (b) handler recovers without progress → **CONFIRMED as the spin mechanism**, but it
  is downstream. **Primary root = the guest ran out of memory on a MemHeap at the
  title screen**, and the retail allocator's non-halting failure path corrupts into
  the 0xFFFFFFFC store.

**What actually needs answering next (for L2/L3):** *why is the heap exhausted here?*
The faulting heap is one of the Milo heaps ("system" / "physical"); `mName` is at
`8(r25)` at fault time but r25 is a runtime pointer (not statically knowable).
Actionable: instrument Xenia to log `mName` (guest read of `[[heap]+8]`) at the
"Allocation failure" `MakeString` call (0x827bcb94) or at the fault, to name the
exhausted heap and the requested `wantBytes` (= `sizeWords*4`, r26<<2). Likely levers:
RB3DX Deluxe (rbdxcache/extra song cache) inflating a heap vs. Xenia's physical/title
memory provisioning, or an earlier emulated-path over-allocation/leak. `last_rip=
0xA00C8B67` is a **host** JIT-cache address (the compiled block for this guest code) —
not independently actionable; the guest instruction above is the truth.

Checkpoint: `/tmp/rb3dx-wf/l1-disasm.json`. Image: `/tmp/rb3dx-wf/rb3dx_decomp.bin`.

---

## L3 — Title background magenta/green "scanline-interlace" corruption

**Verdict: CAPTURE-PATH artifact (tiled-surface headless readback), NOT a genuine
GPU/shader render bug. Does NOT block navigation to main_hub.** (confidence: high)

Frame: `rb3-verify/frames/rb3dx-milestone/02_rb3dx_titlescreen_corrupt.png` — the
RB3-Deluxe logo + background flares are fully legible *through* a fine
magenta/green vertical-stripe scramble.

### Measurements (from the corrupt PNG + the clean splash)
- **Scramble is purely HORIZONTAL, period = exactly 8 pixels.** Row autocorrelation:
  col-shift 8 = **0.975**, shift 16/32 ≈ 0.95, shift 1 = 0.46, shift 2/4 ≈ 0.
  Vertical is clean: row-shift 1 = 0.970, decaying smoothly, no special period.
  8 px × 4 B/px = **32 bytes = the Xbox 360 4bpp micro-tile unit**.
- **Channels are redistributed across pixels within each 8-col group** (magenta = R+B,
  green = G, cyan = G+B, yellow = R+G). Byte-granular, not a whole-pixel permutation
  — matches the documented Xenos 4bpp tiling in `texture_util.h`: *"each 16 bytes
  contain blocks laid out sequentially … 4bpp - odd 4 blocks = even 4 blocks + 32
  bytes."* That layout, read linearly, produces exactly this per-8-pixel channel
  scramble while leaving adjacent rows (similar content) looking clean.
- **Content is intact** — logo/flares readable → the rendered scene is correct; only
  its *memory-layout interpretation* in the readback is wrong.

### Why the splashes are clean (control)
The photosensitivity splash's **white text reads as clean sequential white**
(`246,246,246` with normal anti-aliasing), zero channel-scramble → that frontbuffer
is genuinely **linear**. (Refutes "tiling is always present, just hidden on flat
content" — busy white text would show the scramble and doesn't.) The difference is
upstream of the readback: the splash is a 2D/linear surface; the animated 3D title is
a **resolved-EDRAM frontbuffer stored tiled**.

### Logged params (patched_advance.log, frames 100–500)
```
RequestSwapTexture: 1280x720 fmt=6 endian=0 guest_swizzle=0xA0A host_swizzle=0xA0A
Frame N readback: host_swizzle=0xA0A R<-2 G<-1 B<-0 A<-5
RSTAB: CAPTURE frame=N ... frontbuffer=0x1D1C8000 nonzero_pct=97..100 verdict=SCENE
```
- `fmt=6` = **k_8_8_8_8** (4 B/px) → readback's `w*h*4` sizing is correct; NOT a
  bytes-per-pixel mismatch.
- swizzle `0xA0A` = R←byte2, G←byte1, B←byte0, A←1 = **BGRA→RGBA reversal, harmless**
  (already applied on the CPU, L1666-1704). It cannot produce stripes.
- `frontbuffer=0x1D1C8000` == base_page `0x1D1C8 << 12` — the VdSwap frontbuffer
  itself (double-buffered with `0x1CE30`).
- **The RSTAB `verdict=SCENE` (nonzero-px count) is unaffected by the corruption** →
  L1/L2's scene-detection is not impeded.

### Root cause
`src/xenia/gpu/vulkan/vulkan_command_processor.cc` readback (L1548-1759) **raw-copies
`swap_image`** (`vkCmdCopyImageToBuffer`, L1608) — NOT the untiling `swap_view`
(fetched L894) — and treats the bytes as linear R8G8B8A8. Clean uploaded textures
(splash) pass through `LoadTextureData`'s untile step; the **GPU-resident resolved
frontbuffer skips it** (marked resolved/gpu_written; see the comment at L1537-1542
"do NOT re-upload from guest memory"), so the copied buffer still holds **tiled**
bytes. Swizzle is handled on the CPU here, but **tiling is not.**

### Fix (capture-harness only — zero risk to DC3 / emulation; behind `headless_frame_dump_`)
- **Preferred:** instead of raw-copying `swap_image`, blit through `swap_view`
  (already fetched, swizzle+format correct) into a scratch **linear** RGBA8 VkImage
  (`vkCmdBlitImage` or a fullscreen sample-blit), then `vkCmdCopyImageToBuffer` that.
  Sampling via the view uses the same pipeline as the on-screen present, so the
  readback matches what a real display shows.
- **Fallback (CPU untile):** when the swap texture's `key.tiled` is set, copy the
  **full tiled subresource** (aligned pitch = `align(1280,32)=1280`, aligned height =
  `align(720,32)=736`; tiled addresses incl. bank/pipe bits exceed `w*h*4`, which is
  why a naïve `w*h` offline untile fails) and gather each output texel via
  `texture_address::Tiled2D(x, y, pitch_aligned, /*bpp_log2=*/2)`. Add
  `GetLastSwapTiled()` / `GetLastSwapPitch()` accessors mirroring
  `GetLastSwapHostSwizzle()`.

### Impact on the milestone
**None for navigation.** Input is scripted and the SCENE verdict still fires, so
L1/L2 can drive past the title regardless. If `main_hub` is also a 3D-resolved scene
it may capture with the same scramble (cosmetic to human review of the PNGs only);
2D/menu-composited surfaces will likely capture clean like the splashes. Fix is
optional polish for readable capture frames, not a blocker.

Checkpoint: `/tmp/rb3dx-wf/l3-render.json`.

---

## DECISION (synthesizer, 2026-07-08)

The four lanes converge on a single, non-contradictory picture:

- **L1 (root):** guest heap **OOM** inside `MemHeap::Alloc` @`0x827bca78`. Retail
  `MILO_FAIL` is a no-op, so the OOM failure branch falls through the inlined
  free-block **split** code with sentinels `{mBlock=NULL, mPadWords=0x7FFFFFFF}` →
  `stwux r27,r30,r10` @`0x827bcbd8` stores at `0 + (0x7FFFFFFF<<2)` = guest
  **`0xFFFFFFFC`**. No r2/SDA, no mistranslated register — the bad EA is a
  deterministic function of *"this heap is out of memory."*
- **L2 (amplifier):** Xenia's recovered-fault path **resumes without advancing the
  guest PC** → the store re-executes forever at ~11,640/s. Constant host rip
  `0xA00C8B67` + constant guest PC `0x827BCBD8`. `VdSwap` stops exactly when the
  spin starts → the presenting thread is the spinning thread → render stall.
- **L3:** the magenta/green title corruption is a **headless tiled-readback
  artifact**, not a render bug, and does **not** block navigation.
- **L4:** title→hub input is **START**; the same-instrument A/B is a title-gated,
  default-off runtime memory patch (ready once the wedge clears).

### Why RB3DX doesn't OOM on real hardware

A store to `-4` is fatal on a real 360 too, and RB3DX is a *shipping mod* that
does **not** crash at its own title screen. Therefore the heap is exhausted **only
under emulation** — an emulation-induced OOM, not a game bug. The last `VdSwap`
lands immediately after `xeXamContentCreate root='rbdxcache' cache_size=0
content_size=0` + `NtCreateFile rbdxcache:\rbdxcache` → the **rbdxcache /
XamContentCreate path** is the prime suspect for inflating a heap (or handing the
Deluxe patch a bad size). The VdSwap frontbuffer ring **wraps**
(`0xBEFAA008`→`0xBE04C67C`, ~16 MB / ~30 buffers) → a swapchain **leak is refuted**.

### Fix — two parts

**Part 1 (SHIPPED — general, DC3-inert emulator fix).** The genuine defect L2
confirmed is **resume-without-progress**. Fix = **fault-livelock circuit-breaker**
in `src/xenia/base/exception_handler_posix.cc`, `ExceptionHandlerCallback` (SIGSEGV
case, right after `last_fault_rip_.store`, ~line 168). Track `thread_local`
`(prev_rip, prev_addr, repeat)`; on identical consecutive faults increment; at the
limit `XELOGE(guest EA + host rip)` then `xe::FatalError`. New cvar
**`--fault_spin_limit=4096`** (`DEFINE_uint64`, default 4096, `0` disables,
category CPU). Placed at the **common choke point** so it is path-agnostic (works
whether the "recovery" is the read soft-fault, a GPU write-watch callback, or
restore-and-retry). **DC3-safe:** DC3's legit soft-faults always advance rip or
change the address (they never repeat the same `(rip,addr)` 4096× in a row), and
DC3 still boots to gameplay unchanged; cvar-gated as a backstop. This converts the
silent 11,640/s wedge into a **one-shot diagnosable crash** — it does *not* by
itself reach `main_hub`.

**Part 2 (REACH main_hub — root fix).** Only removing the OOM reaches the hub:
1. Build + boot with Part 1 → expect a clean `FAULT LIVELOCK … guest EA FFFFFFFC`
   abort instead of the spin.
2. **Name the exhausted heap + request size.** Trap the failure-path
   `MakeString("Allocation failure, heap \"%s\", want %d bytes")` @`0x827bcb94`
   (or the fault) and read guest regs: **`r25` = `MemHeap*`** (`mName = [[r25]+8]`;
   names `"system"`@`0x82065aa4` / `"physical"`@`0x82103aec`), **`r26` = sizeWords**
   (`wantBytes = r26<<2`). `r25/r26` are PPC non-volatiles and call args are
   ABI-synced at the call, so read them via the thread `PPCContext`
   (`mmio_handler.cc` has `Memory*` to resolve the guest string).
3. **Correct the provisioning/allocation** for the named heap, title-id-gated
   (`0x45410914`) + cvar so DC3 (`0x373307D9`) stays inert: grow that heap's/title
   physical provisioning, or fix the `rbdxcache`/`XamContentCreate` sizing that
   inflates it.

**Rejected:** page-0-style map/zero-fill of the top page — a benign zero return is
exactly what perpetuates the guest spin (L2), so it keeps wedging instead of
crashing-and-diagnosing.

### Commands

Diagnostic boot (after building Part 1):
```
build/bin/Linux/Checked/xenia-headless --target=/tmp/rb3dxboot/default.xex \
  --protect_zero=false --gpu=vulkan --local_user_count=2 --fault_spin_limit=4096 \
  --headless_timeout_ms=60000 2>&1 | tee /tmp/rb3dx-wf/boot_cb.log
# Expect: "FAULT LIVELOCK: ... guest EA FFFFFFFC ... aborting"  (no 11,640/s spin)
```

Reach main_hub + drive to instrument-select (only after the OOM root fix; input is
START, times re-tuned on the first non-wedged boot):
```
build/bin/Linux/Checked/xenia-headless --target=/tmp/rb3dxboot/default.xex \
  --protect_zero=false --gpu=vulkan --local_user_count=2 --fault_spin_limit=4096 \
  --scripted_input="$(cat rb3-verify/scripts/rb3dx_title_to_guitar.txt)" \
  --dump_frames_path=/tmp/rb3dx-wf/nav --headless_capture_interval=40 \
  --headless_timeout_ms=200000
```

Checkpoint: `/tmp/rb3dx-wf/decision.json`.

---

## L5 — IMPLEMENTATION (2026-07-08): Part 1 shipped, OOM root-caused to heap "main" + corrupted size

### Part 1 — circuit-breaker: BUILT + VERIFIED, DC3-proven-inert

`src/xenia/base/exception_handler_posix.cc`, `ExceptionHandlerCallback` SIGSEGV
case. Implemented as the decision specified, with **two corrections found in
testing**:

1. **`xe::FatalError` from the signal handler HANGS** (it calls `std::exit` →
   atexit/dtors from inside a guest thread's signal frame → deadlock; the process
   ran to the 60 s timeout instead of dying). Replaced with: capture state → set a
   `livelock_tripped_` flag → **park the faulting thread in a `nanosleep` loop**
   (async-signal-safe) so the spin stops, and let the **main headless thread**
   detect the flag and `std::_Exit(70)` cleanly.
2. Verified firing: `FAULT LIVELOCK: 4097 consecutive recovered faults at host rip
   00000000A00C8B67, fault addr 00000001FFFFFFFC (guest EA FFFFFFFC)`. SIGSEGV
   count freezes at 4097 (spin stopped) instead of climbing at 11,640/s.

**DC3-safety PROVEN, not just argued:** DC3 (`0x373307D9`) boots identically with
`--fault_spin_limit=4096` (default) and `=0` — same teardown, `SIGSEGV=0`
throughout, the circuit-breaker never fires (it requires 4096 *identical
consecutive* faults; DC3 has zero). Logs: `/tmp/rb3dx-wf/dc3_safety.log`.

### Reliable fault diagnostics (new, livelock-gated → DC3-inert)

Added to the exception handler + headless status loop. On the livelock trip we
snapshot the faulting thread's **guest `PPCContext*` (host `rsi`, reserved by the
x64 backend for all JIT execution)** and all 16 host GPRs, then decode from the
main thread. Key realization: **`ctx->r[]` is NOT reliable at the fault** (Xenia
caches guest regs in host regs mid-block; `r25/r26` read as garbage and vary per
run), and `r25/r26` are **dead** at the fault (not in host GPRs either). The
reliable channel is the **guest stack**: `r1` (guest SP) *is* synced in the
context, and `MemHeap::Alloc` builds the failure String at `[SP+0x60]`.

**Result — reliably recovered every OOM run:**
```
>>> string via [sp+0x68]->0x48E33350: "Allocation failure, heap "main", want 469767460 bytes"
guest SP(r1)=0x7018F780   stat locals: [sp+0x50]=43549972 [sp+0x54]=43103284 ...
```

### ROOT CAUSE (refines L1): heap **"main"**, size is **corrupted in the top byte**

- The exhausted heap is **"main"** (a large heap, backing region ≈`0x40000000`),
  not "system"/"physical".
- The requested `wantBytes` is `0x??001524`: **low bits `0x1524` (= `0x0549`
  words ≈ 5.4 KB) are correct and STABLE across runs; the TOP BYTE
  (bits 24–31) is garbage and VARIES per run** (`0x04/0x05/0x07/0x11/0x1A/0x1C/
  0x20/0x44` million). So `r26 = 0xGG000549`. The Milo "main" heap gets a spurious
  ~256 MB–1 GB request → the free-block search finds nothing → the L1 fall-through
  store to `0xFFFFFFFC`.
- **Varying-per-run garbage ⇒ reading uninitialized guest memory**, not a
  deterministic JIT-logic bug (a logic bug would repeat the same garbage). Zero on
  real 360 (zeroed pages), garbage under Xenia. This is the emulation-induced OOM.
- The caller is a **virtual-call chain** (return addrs on the frame: `0x8242007C`,
  which sits after a `bctrl` vtable call at `0x82420078`, and `0x827BB8C4`).
  `QueryFunction` doesn't resolve them (symbols are address-only / not JIT-compiled
  in the DB), so the exact size-producing instruction is not yet pinned.

### Experiment RUN + REFUTED: force-zero guest commits

Hypothesis: the garbage top byte is an uninitialized field in a fresh allocation
Xenia failed to zero (console hands out zeroed pages). Implemented
`--rb3dx_force_zero_commit` (title-gated to `0x45410914`, DC3-inert), zeroing
**both** `NtAllocateVirtualMemory` commits (ignoring `X_MEM_NOZERO`/`was_commited`)
**and** `MmAllocatePhysicalMemoryEx` (which stock Xenia never zeroes).
**Result: did NOT fix it** — ~40 % of boots still OOM with it ON
(virtual-only 2/6; virtual+physical 3/8). ⇒ the corrupted byte is **not** from a
fresh commit Xenia skipped; it is **guest-internal Milo-heap reuse** (memory the
game manages, that Xenia cannot zero) **or** a timing-sensitive JIT issue in the
virtual-call chain. Cvar left in-tree **default OFF** as a console-accuracy /
diagnostic lever; it is not the fix.

### Non-determinism (important)

The OOM is a **race**: ~40–60 % of boots wedge at `rbdxcache`; the rest reach the
**animated title and sit there** (frames keep swapping, `SIGSEGV` stays low, no
`main_hub` with the current nav). Frame `2400` of a non-OOM run
(`/tmp/rb3dx-wf/view_2400.png`) shows the title with the L3 magenta/green readback
scramble. So even a non-wedged boot does not currently advance title→hub — the nav
(START at 28/34 s) either isn't consumed or the title needs a further gate; this is
only investigable once the OOM race is removed.

### Status / next

- **Reached `main_hub`: NO.** Part 1 converts the wedge into a clean, diagnosable
  crash (done). The OOM root (heap "main", top-byte-garbage size) is now **named
  and reliably reproducible**, but the exact corruptor (which guest write leaves the
  top byte non-zero, in the virtual-call allocator chain) is unresolved — force-zero
  refuted the fresh-alloc theory, pointing at guest-internal reuse or a JIT edge.
- **Next lever:** trap `MemHeap::Alloc` **entry** (`0x827bca78`, where `r3`/`r4`
  are live args) — a guest breakpoint or a small JIT-entry hook — to log `r4`
  (size) and the caller LR on **every** call, catching the first call whose top
  byte is non-zero and its immediate caller; then disasm that caller's size math.
  Alternatively single-step/trace the `bctrl@0x82420078` virtual target.

Files (this session): `exception_handler_posix.cc`, `exception_handler.h`,
`app/emulator_headless.cc` (livelock diagnosis), `kernel/xboxkrnl/xboxkrnl_memory.cc`
(force-zero cvar). Logs/frames: `/tmp/rb3dx-wf/boot_cb.log`,
`boot_msg2_1.log`/`boot_caller_5.log` (failure message + caller frame),
`dc3_safety.log`, `impl-frames/`. Checkpoint: `/tmp/rb3dx-wf/impl.json`.
