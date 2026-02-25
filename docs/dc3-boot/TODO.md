# TODO: DC3 Boot Progression

*Last updated: 2026-02-25 (Session 38 — manifest address automation COMPLETE, stub audit done)*

Items are ordered by priority. Check off as completed. Add new items each iteration.

---

## Completed: Manifest Address Automation (Session 38)

- [x] **Automate Dc3Addresses from manifest** [xenia + dc3-decomp]
  - Added `address_catalog` section to `generate_xenia_dc3_patch_manifest.py` (73 entries)
  - Added `ADDRESS_CATALOG` dict mapping kAddr field names → MAP symbols
  - Added PE section parsing for `.idata` + computed fields (`g_hash_table`)
  - Made `kAddr` mutable, added `Dc3PopulateAddressesFromCatalog()` called before `ApplyDc3HackPack`
  - 14 `PatchStub8` calls converted to `PatchStub8Resolved` (manifest-resolved with hardcoded fallback)
  - New stubs added to `HACK_PACK_STUBS`: Splash (4), LiveCameraInput (2), HasFileChecksumData,
    VoiceInputPanel, Fader, ShellInput, UIScreen, MoveMgr, ObjRef, list clear
  - `__RTDynamicCast` and `gNullStr` moved into kAddr + catalog

## Completed: CRT `__xc` / `gStringTable` Initialization (Session 37)

- [x] **Disassemble and characterize `Symbol::PreInit`** [analysis]
- [x] **Trace `PreInit` internal allocator behavior** [xenia + analysis]
- [x] **Fix `gStringTable` initialization path** [xenia]
- [x] **Revalidate CRT completion to `main()`** [test]

## Completed: File system assertions (Session 37 cont.)

- [x] **Fix File_Win.cpp drive assertion** [xenia]
- [x] **Fix File.cpp iRoot assertion** [xenia]

---

## Stub Audit: Root Cause Analysis (~200 patches total)

The hack pack currently applies ~200 guest-code patches. They break down into
three tiers based on whether they can ever be removed:

### Tier 1: Permanent for headless mode (~130 stubs) — NO ACTION NEEDED

| Category | Count | Why permanent |
|---|---|---|
| NUI/Kinect SDK | ~70 | No Kinect hardware in xenia |
| Holmes debug network | ~40 | Dev network doesn't exist |
| XBC/SmartGlass | 3 | No SmartGlass in xenia |
| GPU init (null GPU) | ~11 | No GPU in `--gpu=null` mode |
| XMP music | 2 | No media player |
| Bink video | 2 | Video codec needs GPU thread |

These are the cost of running a Kinect dance game without Kinect. The manifest
automation (Session 38) ensures they survive XEX rebuilds.

### Tier 2: Decomp build artifacts (~25 stubs) — FIXABLE VIA DECOMP/LINKER WORK

These exist because `/FORCE:MULTIPLE` linking corrupts data structures:

| Stub | Root cause | Fix path |
|---|---|---|
| CreateDefaults bl-patches (2) | /FORCE duplicates → RndCam dtor crash | Resolve LNK4006 duplicates |
| Object factory overrides (3) | Heap unavailable during CRT `__xc` init | Fix CRT init ordering or resolve missing symbols |
| StringTable::Add override | Same: heap not ready during `__xc` | Same |
| gConditional sentinel write | /FORCE reorders STLport sentinel | Resolve LNK4006 |
| Fader::UpdateValue | Corrupt std::set (SynthInit skipped) | Fix XAudio2 deadlock (see Tier 3) |
| UIScreen::HasPanel | Corrupt std::list from .milo load fail | Fix .milo loading or /FORCE list corruption |
| ObjRef::ReplaceList | Corrupt linked list from .milo fail | Same |
| list<ObjectDir*>::clear | Corrupt linked list from ObjDirItr | Same |
| MoveMgr::Init | Config data missing/corrupt | Fix DTB/config loading |
| ShellInput::Init | SpeechMgr never initialized | Fix NUI subsystem init ordering |
| VoiceInputPanel | Speech config parsing fails | Same |

**Meta-fix**: Resolving the 275 LNK4006 duplicate symbols and 666 LNK2001/2019
unresolved externals would eliminate most of these. This is essentially
"finish the decomp."

### Tier 3: XAudio2/Synth cascade (~8 stubs) — FIXABLE VIA XENIA APU WORK

| Stub | Root cause | Fix path |
|---|---|---|
| Synth360::PreInit | XAudio2 CriticalSection deadlock under nop APU | Fix xenia nop APU CS init |
| SynthInit | Same | Same |
| Fader::UpdateValue | Corrupt std::set because Synth was skipped | Unstubbing Synth fixes this |
| Splash (4) | DirLoader blocks on file I/O | Investigate VFS completion |

**Highest-leverage fix**: If xenia's nop APU could handle XAudio2 CS init without
deadlocking, SynthInit wouldn't need stubbing → Fader corruption vanishes. 3 stubs
gone from one xenia fix.

---

## Current Priority (Iteration 6): Post-Factory Game Init — ACTIVE

Boot now reaches game initialization but hits object factory and config issues.

### Tier 0: Current blockers

- [ ] **Fix object factory instantiation** [xenia + analysis]
  - `Couldn't instantiate class Mat` / `MetaMaterial` / `Cam`
  - sFactories map appears populated but lookup fails
  - Investigate factory registration / init order
  - May be related to /FORCE duplicate symbol addresses

- [ ] **Fix `Data is not Array` cascades** [analysis]
  - Multiple DataNode::Array() calls on non-array data
  - Likely from config parsing — DTB structure may not match expected schema

### Tier 1: High-leverage stub reductions

- [ ] **Investigate XAudio2 CS deadlock in nop APU** [xenia]
  - Root cause: nop APU returns dummy driver handle, XAudio2's CCriticalSectionLock::Enter
    bypasses kernel import (IAT entry zero), auto-init never fires
  - Fixing this would unstub: Synth360::PreInit, SynthInit, Fader::UpdateValue
  - 3 stubs eliminated from one fix

- [ ] **Investigate .milo file I/O completion** [xenia]
  - Several stubs (Splash, UIScreen::HasPanel, ObjRef::ReplaceList) exist because
    .milo asset loading never completes
  - Could be a xenia VFS issue or missing content directory
  - Fixing this would unstub ~6 functions

### Tier 2: Infrastructure / tech debt

- [ ] **Re-check SetupFont literal corruption path** [dc3-decomp + xenia]
  - Verify whether decomp build freshness/literal issue is still present

- [ ] **Re-check heap exhaustion / `MemInit` configuration** [analysis]
  - Reproduce without conflating with the earlier CRT/string-table blocker

---

## Remaining hardcoded addresses (not manifest-resolvable)

These 13 kAddr fields can't be auto-resolved from MAP/PE because they're
instruction-level addresses, LR comparison values, or string literal addresses:

| Field | Why unresolvable |
|---|---|
| `hx_snprintf_vsnprintf_call` | Call site within a function |
| `setup_font_syscfg_return_lr` | Expected LR comparison value |
| `setup_font_ctor1_literal` | .rdata string literal address |
| `setup_font_ctor2_literal` | .rdata string literal address |
| `pooled_font_string` | .rdata string literal address |
| `setup_font_node_source_lr` | LR comparison value |
| `setup_font_node_dest_lr` | LR comparison value |
| `string_reserve_memalloc_ret_lr` | Return address comparison |
| `file_is_local_assert_branch` | Instruction address within FileIsLocal |
| `meminit_assert` | Instruction address within MemInit |
| `memalloc_assert` | Instruction address within MemAlloc |
| `merged_dataarray_node` | ICF merged symbol (no unique MAP entry) |
| `g_conditional` | Static-scope global, no public MAP symbol |

Plus 2 instruction-level patches (`CreateDefaults bl` at specific call sites).
These use hardcoded fallback defaults.

---

## Deferred / Historical Backlog

Older iterations, completed milestones, and deferred post-CRT backlog were moved
to `docs/dc3-boot/ARCHIVED.md` to keep this file focused on the current blocker.

For active non-blocker debt tracking, also use:
- `docs/dc3-boot/HACK_RETIREMENT_MATRIX.md`
- `docs/dc3-boot/CONTINUATION_PLAN.md`
- `docs/dc3-boot/STATUS.md`
