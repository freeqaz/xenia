# Agent E: MAP Address Extraction Script Validation

Date: 2026-02-22

## Script Under Test

`/home/free/code/milohax/xenia/tools/dc3_extract_addresses.py`

## MAP File

`/home/free/code/milohax/dc3-decomp/build/373307D9/default.map` (149,192 lines)

## Results Summary

**Status: PASS -- all addresses validated, no bugs found, no fixes needed.**

### Symbol Resolution

| Category | Count | Found | Missing |
|----------|-------|-------|---------|
| NUI patch symbols (decomp_patches[]) | 62 | 62 | 0 |
| Extra symbols (CRT/thread notify) | 8 | 8 | 0 |
| **Total** | **70** | **70** | **0** |

### Comparison with emulator.cc Hardcoded Addresses

Every address extracted by the script was compared against the corresponding
hardcoded value in `src/xenia/emulator.cc`. Results:

- **decomp_patches[] (62 entries):** All 62 addresses match exactly.
- **kNotifyFuncs[] (2 entries):** Both match (0x830DBBB0, 0x830DBC20).
- **CRT table bounds (4 entries):** All match:
  - `__xc_a` = 0x83B0BB20, `__xc_z` = 0x83B0C138
  - `__xi_a` = 0x83B0C13C, `__xi_z` = 0x83B0C148
- **XapiProcessLock:** 0x83B14C34 -- matches inline comment reference.
- **XapiThreadNotifyRoutineList:** 0x83B14C3C -- matches inline comment reference.

**Zero discrepancies found.**

### Checklist

1. Does it find all 62 NUI function addresses? **Yes.** All 62 patch symbols
   (NUI core, skeleton, image, audio, camera, identity, fitness, wave, head,
   speech, XBC SmartGlass, D3D NUI, and misc) are resolved.

2. Does it find CRT table bounds (__xc_a, __xc_z, __xi_a, __xi_z)? **Yes.**
   All four boundaries match what is hardcoded in emulator.cc's `CrtTable tables[]`.

3. Does it find XapiCallThreadNotifyRoutines and XRegisterThreadNotifyRoutine?
   **Yes.** Both are found, plus the related XapiProcessLock and
   XapiThreadNotifyRoutineList data symbols.

4. Does it handle the new MAP file format correctly (Session 9 regeneration)?
   **Yes.** The parser loaded 147,615 symbol entries from the 149K-line MAP
   file without errors. The MSVC linker MAP format regex handles all symbol
   lines including mangled C++ names, function/import flags, and various
   .obj file references.

### Script Quality Notes

- The script correctly handles MSVC C++ name mangling via `alt_names` and
  substring matching for symbols like `?Initialize@CXbcImpl@@...`.
- Object file disambiguation works correctly when multiple symbols share a
  name (e.g., preferring `nuiimagecameraproperties.obj` over `nuidetroit.obj`
  for camera property functions).
- The generated C++ header output includes ready-to-paste `decomp_patches[]`,
  `kNotifyFuncs[]`, and `CrtTable tables[]` arrays.
- Exit code is 0 when all symbols are found, 1 when any are missing.

### Fixes Made

None. The script is correct as-is.

### Output File

Validated output saved to:
`docs/dc3-boot/agent_e_extracted_addresses.txt`
