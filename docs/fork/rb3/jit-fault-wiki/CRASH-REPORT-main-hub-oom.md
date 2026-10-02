# CRASH REPORT — RB3DX main_hub bring-up: corrupted-size heap OOM in `MemHeap::Alloc`

**Status:** OPEN (root cause established; the exact corruptor is unpinned).
**For:** a fresh investigator with no prior context on this work.
**Sources:** synthesized from `docs/jit-fault-wiki/{06,07,08,09}-*.md`, `INDEX.md`,
`~/.claude/.../project_xenia_seh_fault_wiki.md`, the `/tmp/rb3dx-wf/*.json`
workflow checkpoints, and the boot logs cited inline. No new emulation or
disassembly was run for this report; it consolidates existing evidence.

---

## 1. Executive summary

Rock Band 3 **Deluxe** (RB3DX) runs on a fork of the Xenia Xbox 360 emulator
(`/home/free/code/milohax/xenia`, branch `headless-vulkan-linux`) and reaches its
animated Deluxe title screen, but **never advances to the main menu (`main_hub`)**.
During `main_hub` bring-up, Milo's heap allocator `MemHeap::Alloc` (guest VA
`0x827bca78`, on the heap named **"main"**) is called with a **corrupted size**
`0xGG001524` — the low ~5.4 KB (`0x1524`) is correct and stable, but the **top
byte varies per run** = a read of **uninitialized guest memory** (zero on real
Xbox 360 hardware, garbage under this Xenia fork). The huge size (≈256 MB–1 GB)
exhausts the "main" heap; retail `MILO_FAIL` is a no-op, so execution falls through
into inlined free-block-split code carrying sentinel values and stores to guest
`0xFFFFFFFC`. **Impact:** RB3DX is stuck at the title screen; `main_hub` never
builds, so instrument-select (and the same-instrument feature that depends on it)
is unreachable. (`09` §L1/L5/FINAL; checkpoints `l1-disasm.json`, `impl.json`.)

---

## 2. Reproduction

**Emulator build.** `headless-vulkan-linux` at/after commit `b803faab1`
(circuit-breaker + OOM diagnostics), which builds on the SEH/zero-page fix
`fb864e3e`. Binary: `build/bin/Linux/Checked/xenia-headless`.

**Vehicle.** RB3DX (Deluxe), **not** vanilla clean_tu5. `default.xex` =
`/srv/torrents/games/arbys/rb3/default.xex` (13.97 MB; TitleID `45410914`
v0.0.5.1; has `rbdxcache`; `gen/patch_xbox.hdr` magic `LOLZ` = Deluxe patch
encryption). Boot dir: `/tmp/rb3dxboot` (`default.xex` + symlinks to `/srv` for
`gen`/`charnames`/`AvatarAwards`/`nxeart`). (`09` §L1; memory log lines 59-60.)

**Required flags.** `--protect_zero=false` (the shipped zero-page fix — RB3
harnesses MUST pass it, else the older guest-NULL fault fires; `07`), plus the
diagnostic/circuit-breaker flags:

```
build/bin/Linux/Checked/xenia-headless --target=/tmp/rb3dxboot/default.xex \
  --protect_zero=false --gpu=vulkan --local_user_count=2 --fault_spin_limit=4096 \
  --headless_timeout_ms=60000 2>&1 | tee /tmp/rb3dx-wf/boot_cb.log
```

**Expected:** boot → ESRB + photosensitivity splashes → animated Deluxe title →
`main_hub`. **Actual:** boot reaches the animated title, then `main_hub` bring-up
fails at **VdSwap #600** = `XamContentCreate('rbdxcache'|'globaloptions')`.

**It is a RACE.** Only ~**1 in 5** boots renders sustainably to the title; the
other ~50–80% wedge or stall. (`09` §L5/L6; memory log line 63.)

**Telling the three outcomes apart from logs:**

| Outcome | Signature in the boot log |
|---|---|
| **(a) fault-spin wedge** | `FAULT LIVELOCK: 4097 consecutive recovered faults at host rip …A00C8B67, fault addr …1FFFFFFFC (guest EA FFFFFFFC)`; `SIGSEGV` climbs to ~11,640/s then freezes at 4097 (circuit-breaker parked the thread; clean `_Exit(70)`). Ex: `/tmp/rb3dx-wf/boot_caller_5.log`. |
| **(b) silent render-stall** | VdSwap issuance stops (~#520–609); thereafter only `Thread Status Report … SIGSEGV=0 last_fault=0x0` repeats until timeout. Ex: `/tmp/rb3dxnav/try4/boot.log` (609 VdSwaps, then frozen). |
| **(c) stuck-on-title (lucky boot)** | Thousands of VdSwaps, `SIGSEGV=0`, frontbuffer never leaves the title; e.g. `/tmp/rb3dxnav/g6/boot.log` (11,947 VdSwaps, still cycling title `RSTAB … verdict=SCENE`). |

All three are the **same blocker** — `main_hub` never builds. Input is confirmed
delivered on good boots but there is no menu to advance to (§6). (`09` §L2/L5/L6;
memory log line 63.)

---

## 3. Symptom & evidence

**Three manifestations, one root.** (`09` §L2/L5, DECISION, FINAL.)

**(a) Fault-spin wedge.** Exact signatures (from `/tmp/rb3dx-wf/boot_caller_5.log`
and `09` §L2):
- Host fault addr `0x1FFFFFFFC` = `virtual_membase_(0x100000000) + 0xFFFFFFFC` =
  guest EA **`0xFFFFFFFC`** (= −4, top of address space; above every heap top
  `0xFFD00000` → permanently unmapped).
- Host RIP **constant `0xA00C8B67`** (a JIT code-cache address), guest PC
  **constant `0x827BCBD8`** across every status report → single spinning basic
  block.
- Fault rate **rock-steady ~11,640/sec** (34,900 per 3 s), ~85 µs/fault.
- `VdSwap` stops exactly when the spin begins → the presenting thread is the
  spinning thread → render stall.
- The livelock-gated diagnostic recovers the failure String from the guest stack:
  `>>> string via [sp+0x68]->0x48E33350: "Allocation failure, heap "main", want
  469767460 bytes"` (`469767460` = `0x1C001524` — top byte `0x1C`, low `0x1524`).

**(b) Silent render-stall** at ~frame 520–609: draws stop with no fault at all
(`SIGSEGV=0` throughout). `/tmp/rb3dxnav/try4/boot.log`.

**(c) Stuck-on-title forever** on a lucky non-wedge boot: `/tmp/rb3dxnav/g6/boot.log`
runs 11,947 VdSwaps cycling the animated title (`RSTAB: CAPTURE … verdict=SCENE`)
and never transitions frontbuffers to a menu.

**Frame anchor.** The event is at **VdSwap #600** in both wedge and non-wedge runs:
the wedge run OOMs at #600; the non-wedge run does `XamContentCreate root='rbdxcache'`
then `root='globaloptions'` at #600 and proceeds. (`09` §L6; `l2-faultloop.json`
timeline: last VdSwap `boot2.log:5821`, right after `xeXamContentCreate
root='rbdxcache'` + `NtCreateFile rbdxcache:\rbdxcache`.)

**Best screen reached (pixel proof):** animated Deluxe title —
`rb3-verify/frames/rb3dx-milestone/02_rb3dx_titlescreen_corrupt.png`,
`/tmp/rb3dx-wf/view_2400.png`. A lucky boot also cycles a dark teal-glow 3D scene ↔
blue loading-wipe (`/tmp/rb3dx-wf/drive-frames/{A,B}_*.png`) but **no legible,
interactive `main_hub` was ever captured**. (`09` FINAL.)

---

## 4. Root cause (established)

The chain, mechanical and precise (all guest addresses disassembled from the
decompressed RB3DX image `/tmp/rb3dx-wf/rb3dx_decomp.bin`; `09` §L1,
`l1-disasm.json`):

1. **`MemHeap::Alloc(int sizeWords, int align, int& allocSize)` @ `0x827bca78`**
   (Milo engine `system/utl/MemHeap.cpp`; DC3 reference matches line-for-line, with
   `TryAlloc` **inlined** into `Alloc`). Base-engine code, **not** rbdxcache/Deluxe-
   injected. Identity proven via `mStrategy` 4-way FirstFit/BestFit/LRUFit/LastFit
   switch, the `FreeBlockInfo` scratch layout, and the `"Allocation failure, heap
   \"%s\", want %d bytes"` format string @ `0x82117460`.

2. It is called with **corrupted `sizeWords`** → `wantBytes = 0xGG001524`. Low
   `0x1524` (`0x0549` words ≈ 5.4 KB) is **stable and correct**; the **top byte
   (bits 24–31) is garbage and varies per run** (observed `0x04/0x05/0x07/0x11/
   0x1A/0x1C/0x20/0x44` million). The "main" heap (large, backing ≈ `0x40000000`)
   gets a spurious ~256 MB–1 GB request. (`impl.json`, `09` §L5.)

3. The free-block search finds nothing → the `FreeBlockInfo` scratch keeps its
   sentinel init `{mBlock=NULL, mPadWords=0x7FFFFFFF}`.

4. The OOM failure branch calls `MakeString("Allocation failure …")` +
   `MemPrintOverview` + **`MILO_FAIL(buf)`**. In retail RB3DX **`MILO_FAIL` is a
   no-op/returns**, and because `TryAlloc` is inlined, the free-block **split** code
   sits immediately after the failure branch with **no return/branch between them**
   (failure block ends `0x827bcbb4`, split starts `0x827bcbb8`). So on OOM execution
   **falls through** into the split with the sentinels live.

5. The split computes `newBlock = (int*)mBlock + padWords = 0 + (0x7FFFFFFF << 2)`
   and stores `newBlock->mSizeWords`:
   ```
   0x827bcbd8   7f7e516e   stwux r27, r30, r10      ; EA = r30 + r10 ; *(EA)=r27
   ```
   with `r30 = 0` (`mBlock`, loaded `lwz r30,0x70(r31)` @ `0x827bcb18`) and
   `r10 = r29<<2` where `r29 = 0x7FFFFFFF` (`mPadWords`, `lwz r29,0x7c(r31)` @
   `0x827bcb1c`; `slwi` @ `0x827bcbc0`). **EA = 0 + 0xFFFFFFFC = guest `0xFFFFFFFC`**
   (host `0x1FFFFFFFC`). **No r2/SDA, no mistranslated register** — the bad EA is a
   deterministic function of *"this heap is out of memory."*

6. **Amplifier (Xenia):** the recovered-fault path resumes **without advancing the
   guest PC**, so the store re-executes forever at ~11,640/s. (`09` §L1/L2/FINAL.)

**Why it doesn't happen on real hardware:** a store to `−4` is fatal on a 360 too,
and RB3DX is a shipping mod that does not crash at its own title. Therefore the heap
is exhausted **only under emulation** — an emulation-induced OOM, driven by the
uninitialized top-byte read (which is zero on console). (`09` DECISION "Why RB3DX
doesn't OOM on real hardware".)

---

## 5. The open question (what the investigator must find)

**WHERE is the uninitialized top-byte read, and WHY is it zero on 360 but garbage
under Xenia?**

- The **exact size-producing instruction** in the caller of `MemHeap::Alloc` is
  **not yet pinned**. The low word `0x1524` is stable → a single garbage top byte is
  being OR'd/added into an otherwise-correct base size somewhere in the caller's size
  math. Find that read.
- The caller is a **virtual-call chain**: return addresses seen on the frame are
  `0x8242007C` (immediately after a `bctrl` vtable call at `0x82420078`) and
  `0x827BB8C4`. `QueryFunction` does not resolve these (RB3DX symbols are
  address-only / not JIT-compiled in the DB). (`impl.json`, `09` §L5.)
- **Why garbage under Xenia** is the crux. Candidate mechanisms, none yet confirmed:
  guest-internal Milo-heap **reuse** (memory the game manages, that Xenia cannot
  zero); a BSS/static/struct field Xenia doesn't zero; a **JIT partial-write /
  non-zero-extended load** that leaves the high byte dirty; or a specific allocation
  path Xenia hands out without zeroing. **Varying-per-run garbage rules out a
  deterministic JIT-logic bug** (that would repeat the same garbage). (`09` §L5;
  memory log line 62.)

---

## 6. What's been ruled out

- **`--rb3dx_force_zero_commit` (REFUTED as the fix).** Title-gated to `0x45410914`,
  default OFF. Force-zeroed **both** `NtAllocateVirtualMemory` commits (ignoring
  `X_MEM_NOZERO`/`was_commited`) **and** `MmAllocatePhysicalMemoryEx` (which stock
  Xenia never zeroes). **~40% of boots still OOM with it ON** (virtual-only 2/6;
  virtual+physical 3/8). ⇒ the garbage byte is **not** from a fresh commit Xenia
  skipped; it is guest-internal heap reuse or a JIT edge. Left in-tree as a
  diagnostic lever, **not** the fix. Do not re-run this experiment expecting a fix.
  (`09` §L5, FINAL; `impl.json`.)
- **NOT input / navigation.** Input **is** delivered (scripted START `button=0x0010`
  and A `button=0x1000` produce `Keystroke KEYDOWN` guest pad0 events). START-spam
  across 28–60 s on genuine good boots leaves the game on the title (frontbuffer
  `0x1D1C8000` unchanged for all frames). The title is an **attract screen waiting
  for `main_hub` to finish loading in the background** — you cannot advance to a
  screen that isn't there. (`09` §L4/L6; memory log lines 63-64.)
- **NOT the SEH/zero-page bug.** That earlier fault (guest page-0 store/read, host
  `0x100000000`) is already fixed by `fb864e3e` (cvar-gated hardware-faithful
  zero-page backing; RB3 runs with `--protect_zero=false`). This OOM is a distinct,
  downstream blocker. (`06`, `07`, `INDEX`.)
- **NOT a swapchain leak** — the VdSwap frontbuffer ring wraps
  (`0xBEFAA008`→`0xBE04C67C`, ~16 MB / ~30 buffers). (`09` DECISION.)
- **The title magenta/green scanline scramble is a headless tiled-readback artifact**
  (8-px period = 32-byte Xenos micro-tile), NOT a render bug, and does not block
  navigation. (`09` §L3.)

---

## 7. Recommended investigation levers

Prioritized, all already scoped by prior passes (`09` §L5/L6 "next lever",
`final.json`):

1. **Trap `MemHeap::Alloc` ENTRY at `0x827bca78`** (where `r3 = MemHeap*` and
   `r4 = sizeWords` are **live args**, before the free-block search consumes them) via
   a guest breakpoint or a JIT-entry hook. Log `r4` (size) and the caller **LR** on
   **every** call; catch the **first** call whose size top-byte is non-zero and its
   immediate caller. This is strictly better than trapping the fault, where the args
   are dead (§ctx caveat below).
2. **Disassemble that caller's size math.** Use the decompressed RB3DX image
   `/tmp/rb3dx-wf/rb3dx_decomp.bin` (flat PE, base `0x82000000`, `off = VA −
   0x82000000`; produced by `xex1tool -b` from the retail `default.xex`, which is an
   uncompressed/unencrypted XEX2) and/or the Ghidra service at
   `http://ghidra.local:8001/mcp`. Start at the `bctrl@0x82420078` virtual target and
   the `0x827BB8C4` return site; find where the good base size acquires a dirty high
   byte.
3. **Read guest regs the reliable way.** `ctx->r[]` is **UNRELIABLE at fault time**
   under this JIT (Xenia caches guest regs in host regs mid-block; `r25/r26` read as
   garbage and vary per run, and are **dead** at the fault). The proven channel is the
   **guest stack** via `r1` (synced in the context): `MemHeap::Alloc` builds its
   failure String at `[SP+0x60]` (recovered as `[sp+0x68]->…` in practice). Prefer an
   entry-trap where `r3`/`r4` are live over a fault-time reg read. (`09` §L5;
   `impl.json`.)
4. **Reuse an existing hook precedent** to attach to a guest address — the fork
   already does this in several places: the `dc3_nui` guest-memory patch loop in
   `emulator.cc` (~L2718, patches guest `.text` **before** the JIT compiles it), the
   `ArkFile::Read` PPC trampoline (fork commit `31883d689`), JIT prologue/epilog
   hooks, and the milo-trace tooling. Model the `MemHeap::Alloc` entry trap on one of
   these. (`09` §L4 delivery discussion; memory log.)

Once the corruptor is fixed and the #600 allocation is clean on 100% of boots,
re-run the good-boot START-spam nav (`long_1` already progresses past the title;
`rb3-verify/scripts/rb3dx_title_to_guitar.txt`, input = **START** not A) to drive
`main_hub` → instrument-select.

---

## 8. Constraints on any fix

- **DC3-safe, mandatory.** DC3 (title `0x373307D9`) boots to gameplay on this fork
  and **must not regress**. The shipped `b803faab1` changes are all proven inert for
  DC3 (identical milestones, `SIGSEGV=0`, circuit-breaker never trips; `09` §L5
  table). Any new change must be cvar-gated and/or title-id-gated to `0x45410914`, or
  otherwise proven inert for DC3.
- **Prefer a general emulator-correctness fix** (e.g. correct zeroing/initialization
  of whatever guest memory is being read dirty) over a per-title hack, per project
  direction ("improve the emulator overall"). A **title-gated (`0x45410914`)
  mitigation is acceptable as a fallback** if a general fix proves impractical.
- **Do NOT** map/zero-fill the top page as a "fix" — a benign zero return is exactly
  what perpetuates the guest spin; it keeps wedging instead of crashing-and-
  diagnosing. (`09` DECISION "Rejected".)

---

## 9. Key files & addresses appendix

| Item | Value |
|---|---|
| Emulator repo / branch | `/home/free/code/milohax/xenia` / `headless-vulkan-linux` |
| Zero-page/SEH fix commit | `fb864e3e` (requires `--protect_zero=false`) |
| Circuit-breaker + OOM-diag commit | `b803faab1` |
| Headless binary | `build/bin/Linux/Checked/xenia-headless` |
| RB3DX xex (vehicle) | `/srv/torrents/games/arbys/rb3/default.xex` (TitleID `45410914` v0.0.5.1) |
| Boot dir | `/tmp/rb3dxboot` |
| DC3 xex (regression control) | `/srv/torrents/games/arbys/Dance Central 3/default.xex` (title `0x373307D9`) |
| `MemHeap::Alloc` entry | guest VA `0x827bca78` (`r3=MemHeap*`, `r4=sizeWords`) |
| Faulting store | `0x827bcbd8` `stwux r27,r30,r10` |
| Faulting guest EA / host addr | `0xFFFFFFFC` / `0x1FFFFFFFC` |
| Constant spin host RIP / guest PC | `0xA00C8B67` / `0x827BCBD8` |
| Heap name / size signature | `"main"` / `0xGG001524` (garbage top byte) |
| Caller frame return addrs | `0x8242007C` (after `bctrl@0x82420078`), `0x827BB8C4` |
| Failure fmt string | `@ 0x82117460`; heap names `"system"@0x82065aa4` `"physical"@0x82103aec` |
| Frame anchor | VdSwap #600 = `XamContentCreate('rbdxcache'|'globaloptions')` |
| Decompressed image | `/tmp/rb3dx-wf/rb3dx_decomp.bin` (base `0x82000000`, `off=VA−0x82000000`) |
| cvars | `--fault_spin_limit=4096` (0=off), `--rb3dx_force_zero_commit` (default off, REFUTED), `--protect_zero=false` (required) |
| Ghidra service | `http://ghidra.local:8001/mcp` |
| RB3DX symbols | `/home/free/code/milohax/rb3-xenon/config/45410914/symbols.txt` (address-only `fn_XXXXXXXX`) |
| Emulator src touched | `exception_handler_posix.cc`, `exception_handler.h`, `app/emulator_headless.cc`, `kernel/xboxkrnl/xboxkrnl_memory.cc` |
| Wedge log | `/tmp/rb3dx-wf/boot_caller_5.log` |
| Good-boot-stuck-on-title log | `/tmp/rb3dxnav/g6/boot.log` (11,947 VdSwaps) |
| Silent-stall log | `/tmp/rb3dxnav/try4/boot.log` (609 VdSwaps) |
| Checkpoints | `/tmp/rb3dx-wf/{l1-disasm,l2-faultloop,l3-render,l4-nav,impl,decision,final}.json` |

---

## 10. Known unknowns / contradictions

- **"Store" vs "read soft-fault."** L1 disassembled the faulting instruction as a
  **store** (`stwux`, a write to `0xFFFFFFFC`; `l1-disasm.json`), but L2 describes the
  recovery path that fires as the **"read soft-fault"** at `mmio_handler.cc:518-533`,
  which "zeroes the dest register and resumes" (`l2-faultloop.json`). A store has no
  destination register to zero. Either the recovery classification is loose, or a
  different branch of `ExceptionCallback` actually services this write — **unresolved
  and worth confirming** (it affects which handler the circuit-breaker/root fix should
  target). The circuit-breaker was deliberately placed at the common choke point in
  `exception_handler_posix.cc` to be path-agnostic, sidestepping this ambiguity.
- **Exact size-producing instruction is UNPINNED.** The caller return addrs are known
  (`0x8242007C`/`0x827BB8C4`) but the specific dirty read has not been located.
  Notably, `boot_caller_5.log`'s "code pointers in frame (caller candidates)" and
  guest-stack dump sections printed **empty** in that run — the caller-frame scan did
  not actually resolve caller LRs there, so the caller chain rests on other runs, not
  this log. An entry-trap (§7.1) is the way to nail it.
- **Corruptor mechanism unresolved:** guest-internal heap **reuse** vs unzeroed
  **BSS/static** vs a **JIT partial-write/non-zero-extend**. Force-zero-commit refuted
  the "fresh alloc Xenia skipped" sub-hypothesis but did not discriminate among the
  remaining three.
- **`r25`/`r26` register plan superseded.** `decision.json` (planning) proposed reading
  `r25=MemHeap*` / `r26=sizeWords` via the `PPCContext` at the failure call; `impl.json`
  (post-implementation) found those regs **dead/unreliable at the fault** and switched
  to the guest-stack String. Follow `impl.json` (the stack channel), not the earlier
  register plan.
- **Heap identity evolved.** Early L1 hedged the exhausted heap as `"system"`/
  `"physical"`; the L5 diagnostic definitively named it **`"main"`**. Use `"main"`.
- **Silent-stall vs wedge boundary.** The silent render-stall (try4, 609 VdSwaps,
  `SIGSEGV=0`) and the fault-spin wedge are asserted to share the one root, but the
  silent-stall path shows **no fault at all** — the causal link (why some boots stall
  drawing without ever producing the `0xFFFFFFFC` store) is argued from the shared
  VdSwap-#600 anchor, not independently traced. Confirm if the same corrupted size can
  cause a non-faulting failure path (e.g. an early graceful return the game handles
  differently).

---

## Fable investigation (2026-07-08)

**STATUS: ROOT CAUSE PINNED AND FIX VALIDATED.** The corruptor is a **host-side
Xenia-Linux bug, not guest memory at all**: posix `xe::filesystem::GetInfo()`
(`src/xenia/base/filesystem_posix.cc`) **never assigns `FileInfo::total_size`**
(the Windows version zero-fills the struct and sets it from
`GetFileAttributesEx`). Every `HostPathEntry::update()` therefore copies an
**uninitialized host stack slot** into `entry->size_`, and
`NtQueryInformationFile(FileNetworkOpenInformation)` hands that garbage to the
guest as `end_of_file`. A one-line-class fix (initialize `total_size` from
`stat.st_size`) makes the previously ~50% OOM **0/13 across 13 consecutive
boots**, with RB3DX sailing through the VdSwap-#600 `rbdxcache`/`globaloptions`
wall every time; DC3 A/B is byte-equal on milestones. All evidence below was
gathered live on this fork (probe logs in `/tmp/rb3dx-oom-investigate/`).

### (a) The exact corruptor — full data-flow, instruction by instruction

The report's §5 asked for the size-producing instruction in the caller of
`MemHeap::Alloc`. The full measured chain (RB3DX image = clean-TU5 bytes in all
regions below; verified byte-identical):

1. **Host origin (the actual corruptor):** `HostPathEntry::update()`
   (`src/xenia/vfs/devices/host_path_entry.cc:112`) does
   `size_ = file_info.total_size` where `file_info` is a stack local filled by
   posix `GetInfo()` — **which sets `type` and 3 timestamps but never
   `total_size`** (`src/xenia/base/filesystem_posix.cc:195`, upstream-inherited).
   The slot's residue is typically a stale host pointer: the live probe logged
   `end_of_file` for `path='rbdxcache'` as `0x7FF3D4001520`, `0x7F147C001520`,
   `0x7FEEF4001520`, `0x7F2B68001520` across boots — a **host mmap-region
   pointer varying with ASLR**, whose low bits happen to hold the stable
   `..001520` residue (hence the "stable low word / garbage top byte" pattern;
   the *real* cache file was 32 bytes, so even the 0x1520 low word was garbage
   residue, not the file size).
2. **Kernel boundary:** `NtQueryInformationFile(XFileNetworkOpenInformation)`
   (`src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc`) calls `entry->update()` then
   returns `info->end_of_file = entry->size()` = the garbage. (The fill code
   itself is correct: buffer zeroed, BE fields, EndOfFile at +0x28.)
3. **Guest xapilib:** `GetFileSize` @ **`0x82840070`** → size helper
   @ **`0x8284BC68`**: `NtQueryInformationFile(h, iosb, sp+0x60, 0x38, 0x22)`
   via the statically-linked Nt dispatch table (`[0x82C7AAA8]→0x82C7AA78`,
   entry `+0x20` = the genuine `NtQueryInformationFile` thunk `0x82C4C46C`,
   `sc 2; blr` — live-dumped at fault time, **no RB3DX self-patching**), then
   `ld r11, 0x88(r1)` (EndOfFile @ buf+0x28) and returns the **low 32 bits** —
   `0xGG001520`.
4. **Guest Cache layer:** `CacheXbox::ThreadGetFileSize` @ **`0x827DA7C0`**
   (worker thread; the `NtCreateFile rbdxcache:\rbdxcache disp=1` on thread
   F80000F8 right before every corrupt alloc): `CreateFileA(path,0,1,0,3,0x80,0)`
   @`0x82840240`, `bl 0x82840070` (GetFileSize), then
   `lwz r11,0x160(r30); stw r31,0(r11)` @ **`0x827DA84C`** = `*mData = result`
   → writes `0xGG001520` into **`SaveLoadManager::mSaveSize`** (`this+0x54`).
   Source-identical to DC3's `Cache_Xbox.cpp` `ThreadGetFileSize()`
   (`*data = GetFileSize(file, &fileSizeHigh)`), i.e. `case 0x1e` of the Wii
   decomp's `SaveLoadManager::PollState` (`rb3/src/band3/meta_band/
   SaveLoadManager.cpp:910` `GetFileSizeAsync(..., (uint*)&mSaveSize, ...)`).
5. **The size-producing instruction the report asked for:**
   **`0x82550DE8  lwz r3, 0x54(r30)`** — SaveLoadManager's state-machine
   `case 0x1f` (`mData = _MemAllocTemp(mSaveSize, 0)` then
   `mCache->ReadAsync(name, mData, mSaveSize, 0)`), inside the big state
   function whose entry frame was captured at `entry_sp=0x7018F980`. The
   function references the Symbols `"song_info_cache_name"` (@`0x8209538C`) and
   `"global_options_cache_name"` and the `0x25800` (150 KB)
   `ShowUserSelectUIAsync` constant — **this is the stock TU5 song-info-cache
   load path** (strings exist in vanilla; only the `rbdxcache` content root is
   Deluxe).
6. `_MemAllocTemp` → `operator new` (`bl` ret **`0x827BD028`**) →
   `MemAlloc @0x827BCD38` (`sizeWords = ((size+3)>>2)+1`) →
   `MemHeap::Alloc @0x827BCA78` (single direct caller: `0x827BCF94`) → heap
   "main" OOM → the §4 fall-through → `stwux` @`0x827BCBD8` → guest
   `0xFFFFFFFC`.

**One-boot end-to-end concordance (probe_v4_2.log):** Xenia returned
`end_of_file=0x7F2B68001520` for `rbdxcache` → guest object dump showed
`mSaveSize(+0x54)=0x68001520` → `MemAlloc size=0x68001520` → livelock. QED.

**§10.2 resolved — the caller chain is REPLACED.** The real chain, captured at
`MemAlloc` entry with live args (guest-stack backchain walk):
`0x827BD028` (operator new) ← `0x82550DF0` (SaveLoadManager state machine,
alloc at `case 0x1f`) ← `0x82553ECC` (its Poll driver) ← `0x82270104` ←
`0x82272E94` ← `0x8283CEB0` (main thread). The old `bctrl@0x82420078 /
0x8242007C` candidate is **unrelated** (a vtable call in an engine
`SyncProperty`-class function; stale stack residue), and `0x827BB8C4` is an
incidental saved LR inside the per-thread MemState lookup helper
(`0x827BB898`) that runs on *every* `MemAlloc` — neither identifies the
corruptor.

### (b) Why garbage under Xenia and zero on 360 — mechanism, with evidence

The garbage **never lived in guest memory** — it is **host stack** residue
copied into the VFS entry's `size_` on the host side. This resolves every §10
puzzle at once:

- **`--rb3dx_force_zero_commit` refuted (§6)** because zeroing *guest* commits
  can't touch host-stack garbage — exactly the observed result.
- **Varies per run** because the residue is a host pointer under **host ASLR**
  (and Checked-build stack layout), not because of any guest race.
- **"Race" flavor** (~50% of boots wedge, some stall, some survive): the top
  byte `GG` of the truncated pointer decides the outcome — huge (`0x44+`) →
  immediate OOM → wedge; moderate (`0x04..0x1A`) → 64–450 MB alloc may
  *succeed* → boot proceeds (or degrades later = the silent-stall (b) and
  stuck-title (c) variants; §10.6's shared-root assertion is thereby
  mechanistically explained: same corrupt size, different `GG`).
- **Zero on real 360 / fine on Windows Xenia**: the console kernel returns the
  true file size; Windows `GetInfo` zero-fills + sets `total_size`, so only the
  **posix** build is affected. This also matches DC3-on-this-fork surviving:
  DC3 receives the same garbage sizes on this path (probe showed garbage for
  `gen\patch_xbox.hdr`, `main_xbox.hdr`, `charnames.zbm` too on *every* RB3DX
  boot) but evidently never feeds one into an unguarded allocation this early.

### (c) §10.1 resolved — store vs read soft-fault: it is NEITHER; it's the watch-cleared branch fooled by posix QueryProtect

Instrumented (`RB3DX TOPHOLE`, one-shot, cvar-gated) attribution inside
`MMIOHandler::ExceptionCallback` on a live wedge:

```
RB3DX TOPHOLE: guest EA FFFFFFFC is_write=true branch=watch-cleared-abort detail=3
```

- The faulting access **IS the `stwux` write** (`is_write=true`, L1 was right;
  L2's "read soft-fault zeroes the dest register" description was wrong — that
  branch requires `!is_write` and never fires here).
- The recovering branch is the **"Recheck if the pages are still protected"
  abort** (`mmio_handler.cc` ~:437-446): posix
  `memory::QueryProtect(host 0x1FFFFF000)` returns **kReadWrite (3)**, so the
  handler concludes "another thread cleared the watch, just retry" and returns
  `true` **without touching RIP** → the store re-executes → ~11,640/s livelock.
- Why QueryProtect lies: the guest arena tail is memfd-backed —
  `/proc/<pid>/maps` shows `1e0000000-200000000 rw-s offset=0x100001000
  /memfd:xenia_memory_…` — the **vma permission is rw- but the page still
  faults** (uncommitted memfd page: SIGBUS-beyond-EOF class; the posix
  QueryProtect parses vma perms and cannot see commit state). On Windows,
  `VirtualQuery` reports the true NOACCESS state, the branch is skipped, no
  handler claims the fault, and the process **hard-crashes** — i.e. the
  console-faithful behavior. The resume-without-progress amplifier is
  therefore **also a Linux-specific defect**, and `--fault_spin_limit`
  (`b803faab1`) remains the correct generic backstop for it.

### (d) Fix — validated, general, DC3-proven

**Primary fix (the one that matters), applied in the working tree
(UNCOMMITTED, per investigation scope):** `src/xenia/base/filesystem_posix.cc`
`GetInfo()` — assign **every** `FileInfo` field like the Windows
implementation: `total_size = st.st_size` (0 for directories), plus
`name`/`path` for parity. This is a *general emulator-correctness fix* (the
§8-preferred kind): no cvar, no title gate needed — it replaces uninitialized
garbage with the true value on a path where Windows already returns the true
value.

Validation on this fork (Checked build):
- **Pre-fix:** 8/8 instrumented boots showed the corrupt `0xGG001520`
  songcache alloc (100% corrupt; wedge-vs-survive decided by `GG`), garbage
  `end_of_file` on every host-file size query.
- **Post-fix: 13/13 boots clean** — 0 corrupt allocs, 0 `FAULT LIVELOCK`, 0
  TOPHOLE hits, `end_of_file` exact (rbdxcache=32, patch_xbox.hdr=415639,
  main_xbox.hdr=511281, charnames.zbm=258), `XamContentCreate
  root='rbdxcache'` **and** `root='globaloptions'` both complete (the VdSwap
  #600 wall), ~2100 VdSwaps to the 40 s timeout every run (vs ~930 on wedge
  boots).
- **DC3 (`0x373307D9`) A/B, same command** (`--gpu=null
  --stub_nui_functions=true --headless_timeout_ms=20000
  --dc3_runtime_telemetry_enable=true`): fix vs no-fix **identical** — rc=0,
  355 VdSwaps, SIGSEGV=1 (the known-benign guest-null soft-read at
  `0x82311A94`), clean timeout teardown. DC3-safe by measurement, and by
  construction (strictly more-correct file sizes).
- Upstream note: the `GetInfo` omission is inherited from upstream xenia
  (`filesystem_posix.cc`; a 2023 upstream commit `41c423109` fixed the missing
  `path` in `ListFiles` but nobody noticed `GetInfo::total_size`). Worth
  upstreaming.

**Secondary (optional hardening, NOT applied):** the §10.1 amplifier —
`QueryProtect`-based watch-cleared abort trusting vma perms on Linux. Options,
in preference order: (1) keep `--fault_spin_limit` as the backstop (shipped,
proven); (2) in the `!range` path, treat guest EAs with no owning heap
(`LookupHeap(ea)==nullptr`, i.e. ≥ `0xFFD00000` or the `0x7F000000` hole) as
never-watched → skip the recheck and return `false` (fatal, console-faithful) —
this is wiki 09-L2's "narrow" fix, now with proof it must gate the
**watch-cleared branch**, not the read-soft-fault; (3) make posix QueryProtect
commit-aware (expensive/fragile). Note `Memory::AccessViolationCallback`
dereferences `LookupHeap()`'s result without a null check
(`memory.cc:489-490`) — any EA in a heap hole that got past the recheck would
null-deref inside the signal handler; worth a defensive null check if (2) is
done.

**Diagnostics added (all default-off, gated on new cvar
`--rb3dx_alloc_probe`, title-gated `0x45410914` where guest-visible; left in
tree uncommitted):**
- `emulator.cc`: `MemAlloc` argument probe — an override of the pre-declared
  `__savegprlr_23` (`0x82829244`) with an exact host emulation (std
  r23..r31 + stw r12) plus logging filtered to `lr==0x827BCD40`; logs size,
  align, caller LR (r12), guest-stack backchain, GPRs, live-code integrity
  dumps, and the recovered SaveLoadManager object fields. **Do NOT use
  synthetic PPC caves for hooks**: a 3-instruction cave in .data compiled to
  a no-op-returning function (MemAlloc returned its size argument as the
  pointer — instant heap carnage), and "zero bytes in the image" .data is NOT
  unused (0x82C8D000 is live zero-init pool-allocator state; writing there
  corrupted `FixedSizeAlloc`). The savegprlr-override pattern is the reusable,
  scanner-safe way to observe function entries.
- `xboxkrnl_io_info.cc`: `RB3DX QIF PROBE` — logs every
  `NtQueryInformationFile(NetworkOpen)` path + `end_of_file`.
- `mmio_handler.cc`: `RB3DX TOPHOLE` — one-shot branch attribution for faults
  at guest EA ≥ `0xFFD00000`.

### (e) Still unresolved / follow-ups

- **main_hub pixel confirmation.** The OOM blocker is gone (both #600 content
  creates complete on 100% of boots), but this investigation did not run the
  START-spam nav (`rb3-verify/scripts/rb3dx_title_to_guitar.txt`) to a legible
  main_hub screenshot — that is the next step from §7, now unblocked.
- **Why the stack residue's low word was the stable `0x1520`** (leftover
  pointer arithmetic in a prior host call on that thread) was not chased —
  moot with the fix, but it means the report's "low 0x1524 correct and stable"
  premise was itself an artifact: the real cache file is 32 bytes.
- **Whether the same GetInfo garbage bites other titles/paths** on this fork
  (any guest consumer of NtQueryFullAttributesFile / NetworkOpen /
  XamContent sizes on Linux) — the fix covers them all, but no survey was
  done beyond RB3DX + DC3 boot.
- The **silent-stall (b)** variant was explained mechanistically (small-`GG`
  alloc succeeds) but not step-traced to its stall point; it no longer
  reproduces with the fix (13/13).
