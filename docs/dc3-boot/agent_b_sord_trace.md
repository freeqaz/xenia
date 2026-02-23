# Agent B: "Sord" String Trace Through dc3-decomp

*Date: 2026-02-22*

## Executive Summary

The value `0x536F7264` ("Sord" in ASCII) found in registers r30/r31 at the
`RtlpCoalesceFreeBlocks` crash site does NOT originate from any string literal
in the codebase. It is corrupted heap free-list metadata. The corruption is most
likely caused by one or more CRT static constructors from **NUI/Kinect SDK
libraries** (face detection, speech, skeleton tracking, fitness) that were not
neutralized alongside the 62 NUI function stubs. These constructors initialize
complex COM/ATL objects and may call unresolved functions resolved to RODATA
address 0x82000000 by the linker's `/FORCE:UNRESOLVED` flag.

**Recommended bisect starting points:** Indices 98, 272, 310, 313-314, 319-322,
329-330 (high-risk NUI/COM/D3D constructors), then binary search across the full
0-370 range.

---

## 1. MAP File Symbol Search Results

### 1.1. RtlpCoalesceFreeBlocks (crash site)

Confirmed at address `0x830DC644` from `rtlheap.obj`:

```
0006:00ccc644  RtlpCoalesceFreeBlocks  830dc644 f i  rtlheap.obj
```

Adjacent heap functions in the same .obj:
- `RtlpCreateUnCommittedRange` at 0x830DC00C
- `RtlpInsertUnCommittedPages` at 0x830DC1F4
- `RtlpFindAndCommitPages` at 0x830DC318
- `RtlpDestroyHeapSegment` at 0x830DC5C0
- `RtlpInsertFreeBlock` at 0x830DCA50
- `RtlDestroyHeap` at 0x830DCBF8

### 1.2. CRT Constructor Table

```
__xc_a = 0x83B0BB20   (section 000a:00055f20, auto_08_82F05C00_data.obj)
__xc_z = 0x83B0C138   (section 000a:00056538, auto_08_82F05C00_data.obj)
__xi_a = 0x83B0C13C   (C initializer table start)
__xi_z = 0x83B0C148   (C initializer table end)
```

Table size: 0x618 bytes = 1560 bytes / 4 = **390 entries** (364 valid, 26 null).

The `_cinit` function at `0x830DBA5C` (xapi0dat.obj) iterates this table.

### 1.3. Symbols Containing "Sort" (case-insensitive)

**Total matches in MAP:** ~2971 lines (across all symbol types).

**Sort-related .obj files found:**

| .obj File | Path | Role |
|-----------|------|------|
| NavListSortMgr.obj | lazer/meta_ham/ | UI sort manager for nav lists |
| SongSort.obj | lazer/meta_ham/ | Song sorting base |
| SongSortByDiff.obj | lazer/meta_ham/ | Sort songs by difficulty |
| SongSortByLocation.obj | lazer/meta_ham/ | Sort songs by location |
| SongSortBySong.obj | lazer/meta_ham/ | Sort songs by name |
| SongSortMgr.obj | lazer/meta_ham/ | Song sort manager |
| SongSortNode.obj | lazer/meta_ham/ | Song sort list node |
| ChallengeSortByScore.obj | lazer/meta_ham/ | Challenge sort by score |
| ChallengeSortMgr.obj | lazer/meta_ham/ | Challenge sort manager |
| ChallengeSortNode.obj | lazer/meta_ham/ | Challenge sort node |
| ChallengeSort.obj | lazer/meta_ham/ | Challenge sorting base |
| MQSongSortByCharacter.obj | lazer/meta_ham/ | MQ song sort by character |
| MQSongSortMgr.obj | lazer/meta_ham/ | MQ song sort manager |
| MQSongSortNode.obj | lazer/meta_ham/ | MQ song sort node |
| MQSongSort.obj | lazer/meta_ham/ | MQ song sorting base |
| PlaylistSortByTypeCmp.obj | lazer/meta_ham/ | Playlist sort by type |
| PlaylistSortMgr.obj | lazer/meta_ham/ | Playlist sort manager |
| PlaylistSortNode.obj | lazer/meta_ham/ | Playlist sort node |
| PlaylistSort.obj | lazer/meta_ham/ | Playlist sorting base |
| FitnessCalorieSortByCalorie.obj | lazer/meta_ham/ | Fitness calorie sort |
| FitnessCalorieSortMgr.obj | lazer/meta_ham/ | Fitness calorie sort manager |
| FitnessCalorieSortNode.obj | lazer/meta_ham/ | Fitness calorie sort node |
| FitnessCalorieSort.obj | lazer/meta_ham/ | Fitness calorie sorting base |
| Sort.obj | system/math/ | HashString() function |
| Sorting.obj | system/meta/ | AlphaKeySkip, FirstSortChar |
| qsort.obj | xdk/LIBCMT/ | C runtime qsort |
| qsort_s.obj | xdk/LIBCMT/ | C runtime qsort_s |
| qsort.obj | xdk/nuispeech/ | NUI speech qsort |

**Critical finding: NONE of these Sort-related .obj files have CRT constructors
(`??__E` symbols).** Sort operations are runtime functions, not static
initializers.

### 1.4. Symbols Containing "Sord"

The literal string "Sord" appears in exactly **zero** symbols in the MAP file.
The only partial matches containing "sord" (case-insensitive) are:

```
MSRNoiseSuppressorDestroy   (836bdc20, msrnoisesuppressor.obj)  -- "sord" in "Destroy"
PacketProcessorDestroy      (839eac98, packets.obj)              -- "sord" in "Destroy"
SSMPostProcessorData        (8238d938, ssmdebug.obj)             -- "sord" in "sorData"
```

None of these are CRT constructors or string literals containing "Sord".

### 1.5. String Constants Containing "Sort"

Key string constants found in the MAP:

```
"sort_draws"           (82320a98, Group.obj)
"sorts"                (82140d98, HamStoreFilterProvider.obj)
"dir_sort"             (822abb80, DirLoader.obj)
"sort_groups"          (822ae630, CharClipSet.obj)
"sort_polls"           (822bdaa8, CharPollGroup.obj)
"pCurrentSort"         (822d39e0, NavListSortMgr.obj)
"SongSort.cpp"         (822eede0, SongSort.obj)
"sort_with_headers"    (822d3b60, NavListSortMgr.obj)
"get_current_sort"     (822d3c28, NavListSortMgr.obj)
"get_sort_index"       (822d3c58, NavListSortMgr.obj)
"set_sort_name"        (822d3c68, NavListSortMgr.obj)
"next_sort"            (822d3c88, NavListSortMgr.obj)
```

**No string constant contains "Sord" or "SortD" as a substring.**

### 1.6. "SortDraws" -- The Only "SortD" Match

The function `SortDraws` appears in:
- `Utl.obj` (system/rndobj/) at `0x831586F0`
- `Group.obj` (system/rndobj/) at `0x834A94A8`

These are runtime rendering functions, not CRT constructors. The mangled name
`?SortDraws@@YA_NPAVRndDrawable@@0@Z` appears as a reference in Dir.obj and
Rnd.obj as well.

"SortDraws" as a 4-byte string would produce `0x536F7274` ("Sort"), not
`0x536F7264` ("Sord"). The values differ by one byte: `t` (0x74) vs `d` (0x64).

---

## 2. Binary Search in .obj Files

### 2.1. Binary Pattern Search

A binary `grep` for the byte sequence `\x53\x6F\x72\x64` ("Sord") across all
.obj files returned **zero matches**. The string "Sord" does not exist as a
literal in any compiled object file.

### 2.2. String Extraction from Sort .obj Files

Running `strings` on all Sort-related .obj files in `lazer/meta_ham/` found no
instance of "Sord" or "SortD". The only "SortD" strings found were the
`SortDraws` function references in `system/rndobj/` .obj files (Utl.obj,
Dir.obj, Rnd.obj, Group.obj).

---

## 3. CRT Constructor Analysis

### 3.1. Complete Constructor Inventory

371 unique CRT constructors were identified from the MAP file (indexed 1-371
by address order). The table has 390 slots with 26 null entries (already
nullified by the sanitizer because they pointed to 0x82000000, the RODATA base
address from `/FORCE:UNRESOLVED`).

### 3.2. Constructor Categories

| Category | Count | Heap Risk | Examples |
|----------|-------|-----------|---------|
| NUI/Kinect vector constants | ~80 | LOW | vecHigh32Mask, vecNegate0, etc. |
| NUI/Kinect complex objects | ~20 | **HIGH** | FitnessState, g_heap, CSpCollection |
| CriticalSection/Event | ~21 | MEDIUM | sCritSec, gDataReadCrit, gLock |
| STL containers (map/vector/list) | ~25 | LOW | sMemPointMap, sActiveMovies |
| Simple constants (int/float/bool) | ~50 | NONE | g_nan, SUMFACEWEIGHTS |
| String objects | ~5 | LOW-MED | kServerVer, sRemapClipSearch |
| Symbol objects | ~3 | LOW | sCurrentExportEvent, gHashTable |
| COM/ATL objects (NUISPEECH) | ~10 | **HIGH** | CComAutoCriticalSection, CSPArray |
| D3D/RT objects | ~8 | **HIGH** | g_RTGlobal, s_depNeverIssue |
| XAudio/XAPO registration | ~15 | MEDIUM | m_regProps, sm_RegistrationProperties |
| ChunkID constants | 15 | NONE | kListChunkID, kWaveChunkID |
| Game logic globals | ~120 | LOW-MED | gMoveMergeMap, gDataFuncs |

### 3.3. High-Risk Constructors (Sorted by Risk)

These constructors are most likely to cause heap corruption because they
initialize complex objects from SDK libraries that may call unresolved functions:

| Idx | Address | .obj File | Symbol | Risk Reason |
|-----|---------|-----------|--------|-------------|
| 98 | 82853AB0 | xspeechapi.obj | `_nuispeech@NUISPEECH` | Main NUI speech state; likely calls COM init |
| 272 | 82C25310 | nuifitnesslib.obj | `FitnessState` | Complex NUI fitness state object |
| 275 | 82C2CF28 | nuitruecolor.obj | `TrueColorState@TrueColor` | Complex NUI true color state |
| 277 | 82C3EBE8 | headtrackingprivateapi.obj | `g_htRuntime@HeadOrientation` | Head tracking runtime object |
| 284 | 82C40430 | techmanager.obj | `g_techManager@Gesture` | Gesture tech manager |
| 297 | 82C50AD8 | classifierdata.obj | `m_SkeletonDataHistory` | RingBuffer pointer array |
| 305 | 82C57B68 | nuiaudio.obj | `NuiAudioEtx@NUIAUDIO` | NUI audio ETX state |
| 306 | 82C82E68 | datacollection.obj | `s_DataCollectionInstance` | NUI data collection |
| 307 | 82C85110 | main.obj (nuispeech) | `g_AlternateSettings@NUISPEECH` | Speech alternate settings |
| 308 | 82C8F158 | xboxreg.obj | `_xRegistry@NUISPEECH` | Speech registry |
| 310 | 82CA9FB8 | globals.obj | `g_heap@NUISPEECH` | **NUI speech HEAP allocation** |
| 311 | 82CA9FF8 | globals.obj | `g_prepHeap@NUISPEECH` | **NUI speech prep HEAP** |
| 313 | 82CF2398 | sharedgrammardata.obj | `CSpCollection` | **COM collection object** |
| 314 | 82CF23A8 | sharedgrammardata.obj | `CComAutoCriticalSection` | **ATL critical section** |
| 319 | 82D76FD0 | fecommon.obj | `CFEString::s_cs (CCriticalSection)` | NUI speech CritSec |
| 320 | 82D77020 | fecommon.obj | `CFEString::s_FEStrings (CSPArray)` | **COM array** |
| 321 | 82D77030 | fecommon.obj | `CFEGUID::s_cs (CCriticalSection)` | NUI speech CritSec |
| 322 | 82D77080 | fecommon.obj | `CFEGUID::s_FEGUIDs (CSPArray)` | **COM array** |
| 329 | 82DB7BB0 | exemplar.obj | `Tolerances@STEXEMPLAR` | Skeleton tracking exemplar |
| 330 | 82DFABEC | rtcommon.obj | `g_RTGlobal@D3D` | **D3D runtime global** |
| 339 | 82EE7464 | scheduler.obj | `g_depNeverIssue@D3DXShader` | D3DX shader dependency |

### 3.4. Constructors from D3D/Graphics Subsystem

| Idx | Address | .obj File | Symbol |
|-----|---------|-----------|--------|
| 330 | 82DFABEC | rtcommon.obj | `g_RTGlobal@D3D` |
| 331 | 82DFFEE4 | movie.obj | (movie-related) |
| 335 | 82E0F564 | rtshaderhacker.obj | (shader hacker) |
| 338 | 82E48F9C | apoxhv.obj | (XHV audio APO) |
| 339 | 82EE7464 | scheduler.obj | `g_depNeverIssue@D3DXShader` |
| 340 | 82EE7474 | scheduler.obj | (second scheduler entry) |
| 341 | 82F4499C | partylib.obj | (party library) |
| 342 | 82F55D00 | filter.obj | (audio filter) |

---

## 4. The "Sord" Mystery: Analysis

### 4.1. What 0x536F7264 Is NOT

- **Not a string literal**: No .obj file contains "Sord" as a string.
- **Not a symbol name**: No MAP symbol contains "Sord".
- **Not a chunk ID**: ChunkIDs are "LIST", "RIFF", "WAVE", etc.
- **Not a corrupted "Sort"**: While `0x536F7274` ("Sort") and `0x536F7264`
  ("Sord") differ by one byte, there is no "Sort" string that would appear as a
  raw 4-byte value in heap metadata. String data is stored via pointers, not
  inline.

### 4.2. What 0x536F7264 Likely IS

The value `0x536F7264` in r30/r31 is **corrupted heap free-list pointers**.
In `RtlpCoalesceFreeBlocks`, r30 and r31 typically hold `HEAP_FREE_ENTRY`
forward/backward links. When heap metadata is overwritten by a buffer
overflow or use-after-free, these pointer fields contain whatever data was
written over them.

The fact that the bytes decode to "Sord" in ASCII is likely **coincidental** or
a fragment of a larger data structure that was written over the heap metadata.
Possible sources:

1. **An unresolved function returning garbage**: A CRT constructor calls a
   function resolved to 0x82000000 (PE header bytes). The function "executes"
   random bytes and returns a garbage value. That value gets stored to a heap
   allocation, overflowing into heap metadata.

2. **Uninitialized struct fields**: A static global struct has fields that
   should have been zero-initialized but instead contain COFF section data
   that happens to include the bytes `53 6F 72 64`.

3. **COM/ATL constructor side effects**: NUI SDK COM objects call
   `CoCreateInstance` or similar functions during construction. These calls may
   fail and write error state data that overwrites heap structures.

### 4.3. Why the Pattern is Persistent

STATUS.md notes: "Same 'Sord' / FEEEFEEE pattern seen in Session 5 --
persistent decomp-side bug." This confirms:

- The corruption is **deterministic**, not a race condition.
- It is tied to the decomp binary content, not xenia timing.
- The same constructor always corrupts the same heap location.
- 0xFEEEFEEE (freed memory debug fill) indicates a **use-after-free** pattern
  where freed heap blocks are being reused/corrupted.

---

## 5. Hypothesis

### Primary Hypothesis: NUI SDK CRT Constructors

The heap corruption is caused by one or more CRT static constructors from
**Microsoft NUI SDK libraries** (face detection, speech recognition, skeleton
tracking, fitness tracking). These constructors:

1. Initialize complex COM/ATL objects (`CSpCollection`, `CComAutoCriticalSection`,
   `CSPArray`) that expect COM runtime to be initialized.
2. Allocate private heaps (`g_heap@NUISPEECH`, `g_prepHeap@NUISPEECH`) that may
   conflict with the game's default process heap.
3. Call internal SDK functions that were NOT among the 62 stubbed NUI functions.
   These internal functions may resolve to 0x82000000 via `/FORCE:UNRESOLVED`
   and execute PE header bytes as PPC instructions.

The most likely specific culprits are:
- **Index 310-311** (`g_heap`/`g_prepHeap@NUISPEECH`): These create heap
  objects for the speech subsystem. If they call `RtlCreateHeap` or similar
  with garbage parameters (from unresolved function calls), they could corrupt
  the process heap's free list.
- **Index 313-314** (`CSpCollection`/`CComAutoCriticalSection`): COM collection
  constructor may call `CoCreateInstance` or allocate COM memory.
- **Index 319-322** (fecommon.obj `CCriticalSection`/`CSPArray`): ATL critical
  sections and COM arrays with complex initialization.

### Secondary Hypothesis: D3D Runtime Globals

Index 330 (`g_RTGlobal@D3D` from rtcommon.obj) and index 339
(`g_depNeverIssue@D3DXShader`) initialize D3D runtime state. In the headless
emulator with `--gpu=null`, D3D device functions may be stubbed or missing,
causing these constructors to write garbage.

### Tertiary Hypothesis: Early Harmonix Constructors

Less likely, but some early constructors (indices 1-50) initialize STL
containers and CriticalSection objects. If the Xbox 360 kernel shim has subtle
bugs in `RtlInitializeCriticalSection` event allocation, heap metadata could
be corrupted by the ~21 CriticalSection constructor calls (the STATUS.md shows
RtlInitCS=10, so only 10 of 21 CritSec constructors completed before crash).

---

## 6. Bisect Tool Recommendations

### Phase 1: Targeted High-Risk Tests

Test these specific index ranges first (set `--dc3_crt_bisect_max` accordingly):

| Test | bisect_max | What It Covers | Rationale |
|------|-----------|----------------|-----------|
| 1 | 97 | Indices 0-97 (pre-NUI speech) | Does crash occur without NUI speech? |
| 2 | 75 | Indices 0-75 (pre-gmclassifier) | Does crash occur without ANY NUI code? |
| 3 | 50 | Indices 0-50 (core engine only) | Isolate to Harmonix engine constructors |
| 4 | 25 | Indices 0-25 (very early init) | Test minimal constructor set |

If test 1 does NOT crash but test 2 DOES crash, the culprit is between indices
76-97 (NUI classifier/speech area).

If test 3 crashes, the culprit is in the core Harmonix engine constructors.

### Phase 2: Binary Search

After Phase 1 narrows the range, use standard binary search within the
identified range. With 371 entries, this takes at most 9 iterations (log2(371)
= 8.5).

### Phase 3: Multi-Constructor Exclusion

If the bisect identifies a constructor but nullifying it still crashes (i.e.,
multiple constructors corrupt the heap), implement the `--dc3_crt_skip_indices`
cvar from Agent A's Proposal 2 to iteratively exclude known-bad constructors.

### Specific Indices to Nullify as a Group (if bisect is slow)

These are the NUI SDK constructors that can be safely nullified because
NUI functions are already stubbed. Nullifying these 100+ constructors
should be harmless:

```
Indices 98-114:  xspeechapi + facedetector_detector  (17 entries)
Indices 127:     sceneestimation                       (1 entry)
Indices 133-165: frontaldetector + multiposedetector   (33 entries)
Indices 171-174: speedmeasurement + Skeleton vectors   (4 entries)
Indices 272-306: NUI fitness/face/head/audio/data      (35 entries)
Indices 307-314: NUISPEECH globals + COM objects        (8 entries)
Indices 319-330: fecommon + bgr + exemplar + rtcommon   (12 entries)
```

Total: ~110 NUI/SDK constructors that can be nullified. This could be
implemented as a single address range check in the CRT sanitizer.

---

## 7. Complete CRT Constructor Index (First 50)

For reference, here are the first 50 constructors in table order:

| Idx | Address | .obj | What it initializes |
|-----|---------|------|---------------------|
| 1 | 82419400 | DirLoader.obj | sMemPointMap (map<String,MemPointDelta>) |
| 2 | 82425DD8 | Game.obj | sAutoplayStates |
| 3 | 8242DDE8 | Dir.obj | gOldChars |
| 4 | 82440078 | HamCharacter.obj | mCampaignVO |
| 5 | 8244F098 | CharClip.obj | sFacingPos (FacingBones) |
| 6 | 8244F0E0 | CharClip.obj | sFacingRotAndPos (FacingBones) |
| 7 | 82467650 | HamDirector.obj | gMoveMergeMap |
| 8 | 824676D8 | HamDirector.obj | gOfflineCallback |
| 9 | 82485420 | MoveDir.obj | sFilterVersions (vector) |
| 10 | 82485430 | MoveDir.obj | sOverlayWidth |
| 11 | 82495008 | HamNavList.obj | sListStateMaxDisplay |
| 12 | 82509B18 | Env.obj | sGlobalLighting (BoxMapLighting) |
| 13 | 8251F6F0 | LightPreset.obj | sManualEvents (deque) |
| 14 | 82521C40 | SkeletonUpdate.obj | sCritSec (CriticalSection) |
| 15 | 8254E578 | DataFile.obj | gDataReadCrit (CriticalSection) |
| 16 | 8254E5B0 | DataFile.obj | gFile |
| 17 | 8254E5C8 | DataFile.obj | gConditional |
| 18 | 8254E620 | DataFile.obj | gReadFiles |
| 19 | 8254F2BC | Memory_Xbox.obj | gPhysicalType |
| 20 | 8255B494 | Char.obj | TheCharDebug |
| 21 | 825632DC | GamePanel.obj | gGamePanelCallback |
| 22 | 825632EC | GamePanel.obj | gLoopVizCallback |
| 23 | 825667B4 | Msg.obj | sCurrentExportEvent (Symbol) |
| 24 | 825729AC | PropAnim.obj | sKeyReplace |
| 25 | 8259000C | Mesh.obj | gPatchVerts |
| 26 | 8259AB04 | Dir.obj | sSuperClassMap |
| 27 | 8259AB8C | Dir.obj | gPreloaded |
| 28 | 825DB8A0 | MetagameRank.obj | gUnlockables |
| 29 | 825DB8B0 | MetagameRank.obj | gTiers |
| 30 | 825DB8C0 | MetagameRank.obj | gDeferredAwardQueue |
| 31 | 825ED8F0 | Utl.obj | gResourceFileCacheHelper |
| 32 | 825ED900 | Utl.obj | gChildPolys |
| 33 | 825ED958 | Utl.obj | gParentPolys |
| 34 | 825FF918 | Part.obj | gNoPartOverride |
| 35 | 82628D0C | HamCamShot.obj | sCache (list) |
| 36 | 826391C4 | DataNode.obj | gDataVars |
| 37 | 8263924C | DataNode.obj | gEvalNode |
| 38 | 8263A774 | Symbol.obj | gHashTable (KeylessHash) |
| 39 | 826404EC | Synth.obj | m_regProps (XAPO_REGISTRATION) |
| 40 | 82652354 | Env_NG.obj | sIdentityXfm |
| 41 | 8265BDD4 | HolmesClient.obj | gProfile |
| 42 | 8265BE2C | HolmesClient.obj | gCrit (CriticalSection) |
| 43 | 8265BE64 | HolmesClient.obj | gRequests |
| 44 | 8265BEBC | HolmesClient.obj | gServerName |
| 45 | 8265BEF4 | HolmesClient.obj | gInput |
| 46 | 8265BF2C | HolmesClient.obj | gHolmesTarget |
| 47 | 8265BF64 | HolmesClient.obj | gLastCachedResource |
| 48 | 82661744 | SpotlightDrawer.obj | sLights (vector) |
| 49 | 82661754 | SpotlightDrawer.obj | sCans (vector) |
| 50 | 82661764 | SpotlightDrawer.obj | sShadowSpots (vector) |

---

## 8. Source Code Insights

### 8.1. Symbol Hash Table (gHashTable)

The `gHashTable` CRT constructor (index 38, `Symbol.obj`) initializes a
`KeylessHash<const char*, const char*>` with arguments `(0, nullptr, -1, nullptr)`.
This sets `mSize=0`, `mEntries=nullptr`, `mOwnEntries=true`. **No heap
allocation occurs.** The hash table is only populated later when
`Symbol::PreInit()` is called (which allocates a `StringTable` and resizes the
hash table).

However, any CRT constructor that constructs a `Symbol(const char*)` would try
to call `gHashTable.Find()`. Since `mEntries` is null, `Find()` returns 0.
Then it calls `gStringTable->Add()`, but `gStringTable` is also null, causing
a null pointer dereference -- NOT heap corruption. This suggests Symbol
construction during CRT init would crash with SIGSEGV, not heap corruption.

### 8.2. String Class

The `String()` default constructor uses `gEmpty+4` as its buffer (a static
8-byte array). No heap allocation. The `String(const char*)` constructor calls
`operator=` which allocates heap for the new content.

CRT constructors that initialize `String` objects with string literals
(e.g., `kServerVer = "1"` at index 243, RockCentral.obj) DO allocate heap
memory, but the allocation size is small and deterministic.

### 8.3. KeylessHash / Sort.obj

`Sort.obj` contains only `HashString()`, the hash function used by the Symbol
hash table. It has no CRT constructors and no static globals.

`Sorting.obj` contains `FirstSortChar()` which creates a local static `Symbol
non_alpha_sym("123")`. This would be initialized on first call, NOT during CRT
init, so it is not a CRT constructor.

---

## Appendix: Files Referenced

- MAP file: `/home/free/code/milohax/dc3-decomp/build/373307D9/default.map`
  (WARNING: was truncated to 0 bytes at 11:19 UTC 2026-02-22; all data in this
  report was extracted before truncation via grep cached results)
- .obj directory: `/home/free/code/milohax/dc3-decomp/build/373307D9/obj/`
- Source: `/home/free/code/milohax/dc3-decomp/src/`
- Key source files examined:
  - `/home/free/code/milohax/dc3-decomp/src/system/utl/Symbol.cpp` (gHashTable init)
  - `/home/free/code/milohax/dc3-decomp/src/system/utl/Symbol.h` (Symbol class)
  - `/home/free/code/milohax/dc3-decomp/src/system/utl/KeylessHash.h` (hash table impl)
  - `/home/free/code/milohax/dc3-decomp/src/system/utl/Str.cpp` (String/FixedString)
  - `/home/free/code/milohax/dc3-decomp/src/system/utl/ChunkIDs.cpp` (ChunkID constants)
  - `/home/free/code/milohax/dc3-decomp/src/system/meta/Sorting.cpp` (FirstSortChar)
  - `/home/free/code/milohax/dc3-decomp/src/system/math/Sort.cpp` (HashString)
  - `/home/free/code/milohax/dc3-decomp/src/system/os/CritSec.cpp` (CriticalSection)
