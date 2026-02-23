# Agent D: Unresolved Symbol Categorization

*Generated: 2026-02-22, from `/tmp/link_full.txt` linker output*

## Summary

| Metric | Count |
|--------|-------|
| Total LNK2001+LNK2019 error entries | 1,220 |
| Unique unresolved symbols (after dedup) | 867 |
| Non-string symbols | 334 |
| String literal symbols (`??_C@`) | 533 |

The linker output was captured from a link using `link_no_stubs.rsp` with `/FORCE:UNRESOLVED` removed, causing the Xbox 360 MSVC linker to emit LNK2001/LNK2019 errors for every unresolved external. These symbols are the ones that get silently resolved to address 0 (RVA 0 = guest address 0x82000000, the image base) when `/FORCE:UNRESOLVED` is active, causing runtime crashes.

---

## Category Breakdown

### Priority 0 -- CRT Constructors (CRITICAL, current blocker)

| Category | Count | Fix Strategy | Runtime Impact |
|----------|-------|-------------|----------------|
| `??__E` / `??__F` CRT constructors | 26 | Export from asm obj or implement in decomp | **CRITICAL** -- called during CRT init before `main()` |

These 26 symbols are CRT static initializer functions referenced from `auto_08_82F05C00_data.obj` (the `.CRT$XCU` section). They are function pointers stored in the CRT constructor table (`__xc_a` to `__xc_z`, address range `0x83B0BB20`-`0x83B0C138`).

When unresolved, these pointers become `0x82000000` (image base / PE header) and get "executed" as PPC code, producing garbage return values. The CRT sanitizer in `emulator.cc` already nullifies these 26 entries (they fail the `entry < kCodeStart` check), but any of these constructors may be **necessary** for correct initialization of the globals they construct.

**Detailed list:**

| Symbol | What It Constructs | Correct Address (from MAP) |
|--------|-------------------|---------------------------|
| `??__EgChecksumData@@YAXXZ` | `FileChecksum::gChecksumData` | `0x82EE99F8` |
| `??__E?gThreadAchievements@Achievements@@...` | `Achievements::gThreadAchievements` (vector) | `0x82EE9A08` |
| `??__ETheVirtualKeyboard@@YAXXZ` | `TheVirtualKeyboard` global | `0x82EE9A40` |
| `??__EgEntries@@YAXXZ` | `MessageTimer::gEntries` | `0x82EE9A30` |
| `??__EgOverride@@YAXXZ` | `Keyboard::gOverride` | `0x82EE9A78` |
| `??__ETheMemcardMgr@@YAXXZ` | `TheMemcardMgr` global | `0x82EE9A88` |
| `??__EThePresenceMgr@@YAXXZ` | `ThePresenceMgr` global | `0x82EE9AC0` |
| `??__E?sRoot@FilePath@@0V1@A@@YAXXZ` | `FilePath::sRoot` | `0x82EE9B40` |
| `??__E?sNull@FilePath@@0V1@A@@YAXXZ` | `FilePath::sNull` | `0x82EE9B88` |
| `??__EgDataPointMgr@@YAXXZ` | `DataPointMgr::gDataPointMgr` | `0x82A2E81C` |
| `??__EgWavMgr@@YAXXZ` | `WavMgr::gWavMgr` | `0x82A9CA9C` |
| `??__EkListChunkID@@YAXXZ` | `kListChunkID` RIFF chunk constant | `0x82EE9BE0` |
| `??__EkRiffChunkID@@YAXXZ` | `kRiffChunkID` | `0x82EE9BF8` |
| `??__EkMidiChunkID@@YAXXZ` | `kMidiChunkID` | `0x82EE9C10` |
| `??__EkMidiHeaderChunkID@@YAXXZ` | `kMidiHeaderChunkID` | `0x82EE9C28` |
| `??__EkMidiTrackChunkID@@YAXXZ` | `kMidiTrackChunkID` | `0x82EE9C40` |
| `??__EkWaveChunkID@@YAXXZ` | `kWaveChunkID` | `0x82EE9C58` |
| `??__EkWaveFormatChunkID@@YAXXZ` | `kWaveFormatChunkID` | `0x82EE9C70` |
| `??__EkWaveDataChunkID@@YAXXZ` | `kWaveDataChunkID` | `0x82EE9C88` |
| `??__EkWaveFactChunkID@@YAXXZ` | `kWaveFactChunkID` | `0x82EE9CA0` |
| `??__EkWaveInstChunkID@@YAXXZ` | `kWaveInstChunkID` | `0x82EE9CB8` |
| `??__EkWaveSampleChunkID@@YAXXZ` | `kWaveSampleChunkID` | `0x82EE9CD0` |
| `??__EkWaveCueChunkID@@YAXXZ` | `kWaveCueChunkID` | `0x82EE9CE8` |
| `??__EkWaveLabelChunkID@@YAXXZ` | `kWaveLabelChunkID` | `0x82EE9D00` |
| `??__EkWaveTextChunkID@@YAXXZ` | `kWaveTextChunkID` | `0x82EE9D18` |
| `??__EkWaveAdditionalChunkID@@YAXXZ` | `kWaveAdditionalChunkID` | `0x82EE9D30` |

**Fix strategy:** These constructors exist in the asm .obj files (addresses above are from the successful link). The problem only occurs when the decomp source .obj replaces the asm .obj but doesn't provide these `??__E` dynamic initializers. The jeff toolchain must either:
1. Export these symbols from the asm .obj so they remain linked, or
2. The decomp source must define the static globals with proper C++ initializers (e.g., `FilePath FilePath::sRoot;`).

**Risk assessment:** The 15 `kChunkID` constructors are low-risk (they just initialize 4-byte integer constants). The `FilePath::sRoot`, `FilePath::sNull`, `TheMemcardMgr`, `ThePresenceMgr`, and `TheVirtualKeyboard` constructors are higher-risk because they allocate memory and initialize complex objects -- these are prime candidates for the heap corruption observed in Session 9.

---

### Priority 1 -- Assembly Label References (HIGH, split boundary issues)

| Category | Count | Fix Strategy | Runtime Impact |
|----------|-------|-------------|----------------|
| `lbl_XXXXXXXX` labels | 195 | Fix split boundaries in `splits.txt` | **HIGH** -- called by decomp source code |

These are addresses in the original binary's code/data that the decompiled source code references but that aren't exported from the asm .obj files. They represent functions or data tables at split boundaries where the decomp source calls into asm-only code.

**Address range distribution:**

| Range | Count | Description |
|-------|-------|-------------|
| `0x8201xxxx`-`0x8205xxxx` | 20 | Engine core: Char system, Flow system, Ham system |
| `0x8206xxxx`-`0x820Fxxxx` | 24 | Rendering, physics, third-party (curl, zlib, ogg) |
| `0x8225xxxx` | 1 | StreamRenderer |
| `0x82F0xxxx`-`0x82F6xxxx` | 80 | `.text$dup` section: PropSync templates, Handle dispatch, DataFile, synth |
| `0x8309xxxx` | 2 | HolmesInput (debug tool) |
| `0x830Exxxx` | 38 | Runtime utilities: FormatString, Watcher, UIFontImporter |
| `0x8311xxxx` | 7 | CacheXbox, LightPreset, AutoLoading |
| `0x8316xxxx` | 12 | FreestyleMotionFilter, Profile::Handle |

**Top calling functions** (functions that reference the most lbl_ symbols):

| Count | Function |
|-------|----------|
| 9 | `RndRibbon::SyncProperty` |
| 7 | `CharFeedback::SyncProperty` |
| 7 | `PropSync(Hmx::Matrix3, ...)` |
| 6 | `FlowSlider::SyncProperty` |
| 6 | `PropSync(PracticeStep, ...)` |
| 5 | `PropSync(Sphere, ...)` |

**Fix strategy:** Add the missing addresses to `config/373307D9/splits.txt` so jeff generates asm .obj files that export these symbols, OR implement the referenced functions in decomp source.

The `0x82F6xxxx` cluster (53 symbols) is the highest priority -- these are PropSync template instantiations and data serialization functions that are called during resource loading (before gameplay).

---

### Priority 2 -- Exception Handling Infrastructure

| Category | Count | Fix Strategy | Runtime Impact |
|----------|-------|-------------|----------------|
| `__unwind$NNNNN` handlers | 46 | COFF generation fix in jeff | **MEDIUM** -- needed when exceptions are thrown |
| `__catch$NNNNN` handlers | 2 | COFF generation fix in jeff | **MEDIUM** -- needed for try/catch blocks |

These are PPC structured exception handling (SEH) funclets. The Xbox 360 runtime uses `.xdata`/`.pdata` tables to find unwind and catch handlers. When unresolved, exceptions will crash instead of being caught.

**Affected objects:**

| Object | Unwind Count | What it does |
|--------|-------------|-------------|
| `DebugGraph.obj` | 6 | Debug visualization (low priority) |
| `ScreenMask.obj` | 6 | Screen overlay effects |
| `Morph.obj` | 5 | Mesh morphing |
| `NetStream.obj` | 5 | Network streaming |
| `BoxMap.obj` | 4 | Collision/spatial |
| `TexBlender.obj` | 4 | Texture blending |
| `UIPicture.obj` | 3 | UI image display |
| `FlowLabel.obj` | 3 | Flow graph labels |
| `HamIKEffector.obj` | 3 | Character IK (includes `__unwind__merged_*`) |
| `Song.obj` | 1 (catch) | Song playback |
| `FreestyleMove.obj` | 1 (catch) | Freestyle dance moves |
| Others | 5 | Various |

**Fix strategy:** The jeff toolchain needs to preserve `__unwind$` and `__catch$` symbol exports when generating COFF files from the original binary. These are compiler-generated SEH funclets that must be in the same `.text$x` section as their parent function.

---

### Priority 3 -- Compiler-Merged Data

| Category | Count | Fix Strategy | Runtime Impact |
|----------|-------|-------------|----------------|
| `merged_XXXXXXXX` symbols | 19 | Data extraction in jeff | **MEDIUM** -- constructor init data |

These are data constants that the Xbox 360 MSVC compiler merged across translation units (identical COMDAT folding). They typically contain vtable-like structures, default member values, or floating-point constants used by constructor initialization.

**Detailed list:**

| Symbol | Referenced By | Likely Content |
|--------|-------------|---------------|
| `merged_82013D48` | CharBoneTwist, Dir, MetaPanel | RndDir default init data |
| `merged_82018588` | AnimFilter, CharBlendBone, CharBoneTwist, ContentLoadingPanel, DirLoader | CharBlendBone default init |
| `merged_8204B0D0` | HamPhotoDisplay, Spline | RndSpline default init |
| `merged_8204B0D8` | HamPhotoDisplay, Lit_NG | NgLight default init |
| `merged_8205A368` | UIList | UIList default init |
| `merged_8205A41C` | HamLabel | (HamLabel data) |
| `merged_820B0E2C` | Shockwave, Crowd | WorldCrowd default init |
| `merged_820B13B8` | MultiMeshProxy, CharTransDraw | CharTransDraw default init |
| `merged_82250DB0` | CharDriver, HamIKEffector, KinectSharePanel | HamIKEffector default init |
| `merged_82250DB8` | Gen | RndGenerator default init |
| `merged_82250DBC` | HamCharacter | (overlap with DB0 region) |
| `merged_82250DC4` | HamCharacter | HamCharacter default init |
| `merged_822558A8` | CharIKHead, MainMenuPanel | CharIKHead default init |
| `merged_822558BC` | Character | Character default init |
| `merged_8211FEB4` | TransConstraint | TransConstraint default init |
| `merged_823AAA20` | CharServoBone | CharServoBone init table (5 entries) |
| `merged_823AABD4`-`823AAC64` | CharServoBone | (4 more entries in same table) |
| `merged_823AC1F8` | CharBoneTwist | CharBoneTwist init data |

**Fix strategy:** jeff must extract these merged data blobs from the original binary and emit them as COMDAT sections in the asm .obj files, with the `merged_XXXXXXXX` symbol name exported.

---

### Priority 4 -- Third-Party Library Stubs

| Category | Count | Fix Strategy | Runtime Impact |
|----------|-------|-------------|----------------|
| libjpeg | 7 | Write `jmemnobs.c` stub | **MEDIUM** -- JPEG loading |
| libcurl | 3 | Write stub functions | **LOW** -- network (can be deferred) |
| Ogg Vorbis | 7 | Write stubs or fix splits | **MEDIUM** -- audio playback |
| zlib | 1 | Write `zcfree` stub | **LOW** -- compression |
| Network/Socket | 1 | Write stub | **LOW** -- online features |

#### libjpeg (7 symbols)

All from `jmemmgr.obj` -- these are the platform-specific memory backend that libjpeg requires:

| Symbol | Function |
|--------|----------|
| `?jpeg_mem_init@@YAJ...` | `jpeg_mem_init()` |
| `?jpeg_mem_term@@YAX...` | `jpeg_mem_term()` |
| `?jpeg_mem_available@@YAJ...` | `jpeg_mem_available()` |
| `?jpeg_get_small@@YAPAX...` | `jpeg_get_small()` |
| `?jpeg_get_large@@YAPAX...` | `jpeg_get_large()` |
| `?jpeg_free_small@@YAX...` | `jpeg_free_small()` |
| `?jpeg_free_large@@YAX...` | `jpeg_free_large()` |

**Fix:** Create `src/system/net/libjpeg/jmemnobs.c` implementing the "no backing store" memory model (standard libjpeg pattern -- just forwards to `malloc`/`free`).

#### libcurl (3 unique symbols)

| Symbol | Referenced By |
|--------|-------------|
| `Curl_multi_canPipeline` | sendf.obj, transfer.obj |
| `curl_getenv` | netrc.obj |
| `curlx_sltosi` | parsedate.obj, smtp.obj |

**Fix:** These are internal curl utility functions. Stub with trivial implementations (return 0 / NULL).

#### Ogg Vorbis (7 symbols)

| Symbol | Referenced By |
|--------|-------------|
| `OggFree` | 7 objects (bitrate, bitwise, block, etc.) |
| `floor0_free_info` through `floor0_unpack` | auto_08_82F5DC9C_data.obj |
| `vorbis_lpc_from_data` | block.obj |
| `vorbis_lpc_predict` | block.obj |
| `_vp_ampmax_decay` | block.obj |
| `_vp_global_look` | block.obj |
| `_poll_mapping_P` | synthesis.obj |

**Fix:** `OggFree` is likely just `free()`. The `floor0_*` symbols are function pointers in a vorbis floor codec table -- these need to be exported from the asm. The `vorbis_lpc_*` and `_vp_*` functions need stubs or split fixes.

#### zlib (1 symbol)

| Symbol | Referenced By |
|--------|-------------|
| `zcfree` | deflate.obj |

**Fix:** `zcfree` is the zlib memory free function: `void zcfree(void *opaque, void *ptr) { free(ptr); }`

---

### Priority 5 -- Template Instantiations and Engine Methods

| Category | Count | Fix Strategy | Runtime Impact |
|----------|-------|-------------|----------------|
| ObjPtrList/ObjRefConcrete template instantiations | ~20 | Implement in decomp source | **MEDIUM** -- object system |
| PropSync specializations | ~15 | Implement or fix splits | **MEDIUM** -- property serialization |
| Serialization (Load/PreLoad) | ~30 | Fix splits (most reference lbl_ addresses) | **HIGH** -- resource loading |
| Handle dispatch methods | ~10 | Fix splits | **MEDIUM** -- script system |
| FormatString operators | 4 | Implement `operator<<` overloads | **LOW** -- string formatting |
| String constructors | 2 | Implement `String::String(const char*)` | **HIGH** -- used everywhere |
| Jumptable symbols | 2 | Fix splits for Memory_Xbox.obj | **HIGH** -- memory allocator |

The `ObjPtrList<T>` template needs instantiations for: `FlowNode`, `Hmx::Object`, `RndAnimatable`, `RndDrawable`, `RndTransformable`. Each needs `erase()`, `Link()`, `Replace()`, `RefOwner()`, and `Node::RefOwner()`.

The `ObjRefConcrete<T>` template needs `CopyRef()` for: `ObjectDir`, `MoggClip`, `RndAnimatable`, `RndTex`, `RndTransformable`.

**Fix strategy:** These are all C++ template methods. The decomp source needs to include explicit template instantiation directives, e.g.:
```cpp
template class ObjPtrList<RndDrawable, ObjectDir>;
```

---

### Priority 6 -- Miscellaneous C++ Methods

| Category | Count | Fix Strategy |
|----------|-------|-------------|
| Global variable definitions | 3 | Define in decomp source |
| Constructor/destructor bodies | ~15 | Implement in decomp source |
| Virtual method implementations | ~20 | Implement in decomp source |
| Accessor methods (const getters) | ~10 | Implement in decomp source |
| Platform-specific (CloseHandle, WSA, XNetDns) | 5 | Stub in XDK layer |
| Float constant (`__real@c1f00000`) | 1 | Define float literal |

**Notable missing globals:**

| Symbol | Type | Referenced By |
|--------|------|-------------|
| `?sInstance@HamSongData@@2PAV1@A` | `HamSongData*` | Game.obj, HamDirector.obj, HamSongData.obj |
| `?kStreamEndMs@Stream@@2MB` | `float const` | CreditsPanel, LoadingPanel, MetaMusic, StandardStream |
| `?gDefaultBeatMap@@3VBeatMap@@A` | `BeatMap` | BeatMap.obj |

**Notable missing constructors:**

| Symbol | Class |
|--------|-------|
| `??0Character@@QAA@XZ` | `Character::Character()` |
| `??0HamCharacter@@QAA@XZ` | `HamCharacter::HamCharacter()` |
| `??0RndDir@@IAA@XZ` | `RndDir::RndDir()` (protected) |
| `??0UIList@@IAA@XZ` | `UIList::UIList()` (protected) |
| `??0BeatMap@@QAA@XZ` | `BeatMap::BeatMap()` |
| `??0DeJitter@@QAA@XZ` | `DeJitter::DeJitter()` |

---

### String Literals (533 unique `??_C@` symbols)

These are MSVC COMDAT string literals that the decomp source references but that aren't emitted into the linked output. They are NOT the same as the 639 pre-dedup count; after cleaning up LNK2001 vs LNK2019 duplicates, 533 unique strings remain.

**Top referencing objects:**

| Object | String Count |
|--------|-------------|
| `App.obj` | ~200+ |
| `cconstanttable.obj` | ~100+ |
| `nlsdata2.obj` | ~20 |
| `schema.obj` | ~10 |

**Fix strategy:** This is a jeff COFF generation issue. See Agent C's analysis (`agent_c_comdat_analysis.md`). The `??_C@` symbols need to be emitted as COMDAT `.rdata` sections in the asm .obj files, with the correct IMAGE_COMDAT_SELECT_ANY selection type.

---

## Cross-Reference: CRT Constructor Dependencies

The 26 unresolved CRT constructors (Priority 0) are the **direct cause** of the current heap corruption. Here is the dependency chain:

```
mainCRTStartup()
  -> _cinit()
    -> iterates __xc_a..__xc_z function pointer table
      -> calls each non-null entry
        -> 26 entries point to 0x82000000 (unresolved)
          -> CRT sanitizer nullifies them
            -> globals are LEFT UNINITIALIZED
              -> later code uses uninitialized globals
                -> heap corruption / null vtable crashes
```

**Which CRT constructors can corrupt the heap?**

The ChunkID constructors (`kListChunkID`, `kRiffChunkID`, etc.) are safe to skip -- they initialize 4-byte integer constants that default to 0.

The dangerous ones are those that allocate memory or initialize complex objects:

| Constructor | Risk | Why |
|------------|------|-----|
| `TheMemcardMgr` | **HIGH** | Singleton with heap allocation, lock init |
| `ThePresenceMgr` | **HIGH** | Singleton with heap allocation |
| `TheVirtualKeyboard` | **HIGH** | Singleton with heap allocation |
| `gThreadAchievements` | **HIGH** | `std::vector` with heap allocation |
| `FilePath::sRoot` | **MEDIUM** | String allocation, used by file I/O |
| `FilePath::sNull` | **MEDIUM** | String allocation |
| `gDataPointMgr` | **MEDIUM** | Data collection manager |
| `gWavMgr` | **MEDIUM** | Audio manager |
| `gChecksumData` | **LOW** | Simple data structure |
| `gEntries` | **LOW** | Message timer entries |
| `gOverride` | **LOW** | Keyboard override |
| ChunkID constants (15) | **NONE** | Simple integer init |

---

## Object Files With Most Unresolved References

| References | Object | Category |
|-----------|--------|----------|
| 200+ | `App.obj` | String literals (game initialization paths) |
| 100+ | `cconstanttable.obj` | String literals (D3DX shader constants) |
| 26 | `auto_08_82F05C00_data.obj` | CRT constructors |
| 20 | `nlsdata2.obj` | Locale/NLS strings |
| 13 | `schema.obj` | lbl_ labels (D3DX schema tables) |
| 7 | `PropSync.obj` | lbl_ labels (property sync templates) |
| 7 | `jmemmgr.obj` | libjpeg memory backend |
| 7 | Various | Template instantiations |
| 6 | `auto_08_82F5DC9C_data.obj` | Ogg Vorbis floor0 function table |

---

## Recommended Fix Order

1. **CRT constructors (26):** Export `??__E` symbols from asm .obj or implement static globals in decomp. This unblocks CRT init and is the current boot blocker.

2. **Jumptable + Memory_Xbox lbl_ (4):** `jumptable_820050E8`, `jumptable_82005128`, plus the lbl_ refs. These are in the memory allocator and affect all heap operations.

3. **String::String(const char*) constructor (1):** Referenced by 7+ objects. Without this, any string construction from a C string literal crashes.

4. **Template instantiations (20):** ObjPtrList/ObjRefConcrete methods. These affect the entire object system.

5. **libjpeg stubs (7):** Quick win -- standard `jmemnobs.c` pattern.

6. **Serialization lbl_ refs (30):** Load/PreLoad methods needed for resource loading.

7. **PropSync lbl_ refs (15):** Property synchronization needed for object serialization.

8. **Remaining lbl_ and merged_ (100+):** Fix split boundaries incrementally.

9. **String literals (533):** Jeff COFF fix for `??_C@` COMDAT emission.

10. **Exception handlers (48):** Unwind/catch funclets -- can be deferred until exceptions are actually thrown.

---

## Data Sources

- **Linker output:** `/tmp/link_full.txt` (812KB, captured 2026-02-22)
- **MAP file (successful link):** `/home/free/code/milohax/dc3-decomp/build/373307D9/default.exe.MAP` (209K lines)
- **MAP file (current):** `/home/free/code/milohax/dc3-decomp/build/373307D9/default.map` (149K lines)
- **CRT table addresses:** `0x83B0BB20`-`0x83B0C138` (C++ constructors), `0x83B0C13C`-`0x83B0C148` (C initializers)
- **Image base:** `0x82000000`
- **Code section start:** `0x822C0000` (approximate, used by CRT sanitizer threshold)
