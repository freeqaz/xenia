# Agent C: COMDAT String Literal Analysis

Date: 2026-02-22

## Summary

749 string literal symbols (`??_C@_...`) are unresolved in the dc3-decomp hybrid link.
The root cause is a **two-part failure**: (1) a compiler hash mismatch between decomp and
split objects makes string COMDAT deduplication impossible, and (2) a recent code change
in jeff's xex.rs disabled COMDAT extraction for `.rdata` sections entirely, preventing
even split-internal string deduplication.

---

## 1. How xex.rs Currently Handles COMDAT/String Sections

### COMDAT symbol collection (split.rs lines 1625-1677)

In `split_obj()`, all global defined symbols are marked as COMDAT candidates, with
specific exclusions:
- `lbl_*`, `pdata@*`, `except_data_*`, `except_record_*`, `__unwind$*`
- `??__E*` (CRT dynamic initializers)
- CRT sentinel/array symbols (`__x*_a`, `__x*_z`, `__pioinit`, etc.)
- Save/restore stubs (`__savegprlr*`, `__restgprlr*`, `__savefpr*`, `__restfpr*`)

This means `??_C@` string literal symbols ARE marked as COMDAT candidates and stored
in `obj.comdat_symbols`.

### COMDAT region extraction (xex.rs lines 1325-1377)

The `write_coff()` function iterates all symbols to build `comdat_regions` -- a map of
`(section_index, offset) -> (symbol_index, size)` for regions to extract into COMDAT
sections.

**Critical filter at line 1343:**
```rust
// Only extract code sections to COMDAT. Data/rdata symbols stay in
// their parent sections to preserve relocations (e.g., vtable entries
// that reference COMDAT functions).
if sect.kind != ObjSectionKind::Code {
    continue;
}
```

This means `??_C@` symbols in `.rdata` sections are **skipped entirely** from COMDAT
extraction. They never enter `comdat_regions` or `comdat_extracted_sections`.

### Symbol emission (xex.rs lines 1580-1620)

For symbols with `is_comdat_sym == true`, the code checks `comdat_extracted_sections`
for a matching COMDAT section. Since `.rdata` symbols were never extracted, `comdat_info`
is `None`, and the symbol falls through to the regular emission path at line 1630.

Result: `??_C@` string symbols are emitted as **regular GLOBAL/EXTERNAL symbols** in
the parent `.rdata` section, without COMDAT wrapping (`IMAGE_COMDAT_SELECT_ANY`).

---

## 2. What the Recent Fix Changed

### Commit cf01a80: "Restrict COMDAT extraction to code sections only"

**Before** (broken differently):
```rust
if sect.kind == ObjSectionKind::Bss {
    continue;
}
```
This allowed `.rdata` and `.data` symbols to be extracted into COMDAT sections
(`.rdata$dup`, `.data$dup`). However, vtable entries in `.rdata` that contained
relocations to COMDAT functions had their relocations dropped during extraction,
resulting in null vtable pointers at link time.

**After** (current state):
```rust
if sect.kind != ObjSectionKind::Code {
    continue;
}
```
Only `.text` sections get COMDAT extraction. This fixed the vtable issue but completely
disabled COMDAT for `.rdata` string literals, causing the 749 unresolved symbols.

### Other recent COMDAT changes:
- **d330415** (split.rs): Added CRT initializer splitting and `.CRT` section renaming;
  excluded `??__E*` from COMDAT
- **8de8375** (xex.rs): Preserved REL24 addends for COMDAT offsets
- **7c79b2a** (xex.rs): Hardened COMDAT linking
- **c445657** (xex.rs): Original COMDAT marking for `__unwind$` symbols

---

## 3. Why 749 String Literals Remain Unresolved

The failure is caused by the interaction of two independent issues:

### Issue A: Compiler Hash Mismatch (fundamental, unfixable in jeff)

MSVC `??_C@` symbol names encode the string content plus a compiler-generated hash:
```
??_C@_0BA@JPIJPPAL@CharBonesObject?$AA@   (split/original XEX)
??_C@_0BA@A@CharBonesObject?$AA@           (decomp/recompiled)
```

The decomp compiler (MSVC X360 cl.exe, but different build environment) generates
different hashes than the original compiler that built the XEX. For the SAME string
"CharBonesObject":
- Original hash: `JPIJPPAL` (8 chars, full CRC-based hash)
- Decomp hash: `A` (1 char, simplified/minimal hash)

When a decomp `.obj` replaces a split `.obj` in the hybrid link:
1. The decomp obj defines `??_C@_0BA@A@CharBonesObject?$AA@`
2. Other split objects still reference `??_C@_0BA@JPIJPPAL@CharBonesObject?$AA@`
3. The linker sees the split-hash variant as unresolved

**Quantified impact**: 251 decomp units replace split units. These replaced split
units collectively define ~1,387 `??_C@` symbols. Not all are referenced cross-unit,
but a significant fraction (749) are.

### Issue B: Missing COMDAT Extraction for .rdata (fixable in jeff)

Even if both decomp and split used the same hashes, the current code would emit string
symbols as regular GLOBAL symbols (not COMDAT SELECT_ANY). This would cause LNK4006
duplicate warnings instead of clean deduplication. With the `cf01a80` fix, `.rdata`
COMDAT extraction was disabled to prevent vtable corruption.

### Combined Effect

The 749 errors are **unresolved externals** (LNK2001/LNK2019), not duplicates:
- 110 are LNK2019 (unresolved external symbol referenced in function)
- 639 are LNK2001 (unresolved external symbol)

These occur when split objects reference `??_C@` strings whose definitions lived in
a unit that was replaced by a decomp-compiled version (which defines different hashes).

---

## 4. Proposed Fixes

### Fix 1: Hash-Agnostic String Aliasing (in jeff's xex.rs or split pipeline)

**Approach**: When jeff emits a split `.obj` file, for every `??_C@` symbol, also emit
a **weak alias** using a canonical (hash-independent) name derived from the string
content. The decomp compiler would need to do the same. Both would resolve to the same
definition regardless of hash.

**Problem**: COFF doesn't have weak aliases in the same way as ELF. This would require
a linker script or definition file (.def) approach, which the MSVC X360 linker may not
support for this purpose.

### Fix 2: Re-enable .rdata COMDAT Extraction with Relocation Fix (in xex.rs)

**Approach**: Revert the `cf01a80` restriction, but fix the underlying vtable relocation
issue. The problem was that relocations in `.rdata$dup` COMDAT sections were being
dropped. The fix:

```rust
// In the COMDAT region extraction, instead of blanket-excluding non-code:
if sect.kind != ObjSectionKind::Code {
    continue;
}

// Use a more targeted filter:
// Only exclude BSS (no data to extract) and symbols that have
// complex relocation patterns (vtables, EH data).
if sect.kind == ObjSectionKind::Bss {
    continue;
}
// Skip symbols that are NOT string literals in rdata
// (vtables, RTTI, exception records have relocations that get lost)
if sect.kind == ObjSectionKind::ReadOnlyData
    && !sym.name.starts_with("??_C@")
    && !sym.name.starts_with("__real@")
{
    continue;
}
```

This would allow `??_C@` and `__real@` (float constants) to be COMDAT-extracted into
`.rdata$dup` sections while keeping vtables and other complex `.rdata` structures in
their parent sections.

**This fixes**: LNK4006 duplicate warnings for strings shared across split objects.
**This does NOT fix**: The 749 unresolved errors from hash mismatch between decomp
and split compilers.

### Fix 3: Post-Split String Stub Generation (in dc3-decomp build pipeline)

**Approach**: After jeff splits but before linking, run a script that:
1. Scans all split `.obj` files for `??_C@` symbol references
2. Scans all decomp `.obj` files for `??_C@` symbol definitions
3. For each split-hash reference whose string content matches a decomp-hash definition,
   generates a tiny `.obj` stub that:
   - Defines the split-hash symbol name
   - Contains the string data
   - Marks it as `IMAGE_COMDAT_SELECT_ANY`

This is the most practical fix because:
- It doesn't require modifying jeff's core COMDAT logic
- It can be generated offline as a build step
- It handles the hash mismatch transparently

**Implementation**: A Python script using the `lief` or `pefile` library to:
1. Extract `??_C@` UNDEFINED symbols from split .obj files
2. Decode the string content from the mangled name
3. Create stub .obj files with the correct string data under the split-hash name

### Fix 4: Regenerate Split .asm with Decomp-Compatible Hashes (in jeff)

**Approach**: Modify jeff's symbol naming to use the same hash algorithm as the decomp
compiler. Since the decomp uses MSVC X360 16.00.11886.00, reverse-engineer or replicate
its string literal hash algorithm and use it when generating `??_C@` symbol names in the
split `.s` files.

**Problem**: The hash algorithm is an internal detail of the specific MSVC build. It may
be difficult to replicate exactly, and the decomp hashes (`A`, `A`, `A`...) suggest the
cross-compiler may be using a trivial/degenerate hash.

### Fix 5: Strip Hashes from Both Sides (in jeff + dc3-decomp build)

**Approach**: Normalize all `??_C@` symbol names by replacing the hash portion with a
fixed value (e.g., `A`). In jeff's xex.rs `write_coff()`, when emitting a symbol whose
name matches `??_C@_XX@HASH@content`, rewrite it to `??_C@_XX@A@content`. The decomp
compiler already appears to use `A` as the hash.

**This is the simplest fix** and only requires:
1. A string replacement in jeff's `write_coff()` during symbol name emission
2. The same replacement in the assembly `.s` file generation (for references in code)

```rust
// In write_coff(), when building symbol name:
fn normalize_string_literal_name(name: &str) -> String {
    // ??_C@_XX@HASH@content -> ??_C@_XX@A@content
    if name.starts_with("??_C@_") {
        if let Some(at2) = name[6..].find('@') {
            if let Some(at3) = name[6 + at2 + 1..].find('@') {
                let prefix = &name[..6 + at2 + 1]; // "??_C@_XX@"
                let content = &name[6 + at2 + 1 + at3..]; // "@content..."
                return format!("{}A{}", prefix, content);
            }
        }
    }
    name.to_string()
}
```

---

## 5. Recommended Priority

1. **Fix 5 (hash normalization)** -- Simplest, most robust. Both jeff and decomp would
   use `A` as the hash, achieving symbol name compatibility. Risk: if two different
   strings produce the same `??_C@_XX@A@` prefix (unlikely but possible for same-length
   strings), the linker would merge them incorrectly. The `content` suffix should
   prevent this.

2. **Fix 2 (targeted .rdata COMDAT)** -- Should be done regardless to get proper
   SELECT_ANY dedup for strings within split objects. Reduces LNK4006 warnings.

3. **Fix 3 (post-split stubs)** -- Safest fallback if hash normalization has edge cases.

---

## 6. Files Analyzed

| File | Purpose |
|------|---------|
| `/home/free/code/milohax/jeff/src/util/xex.rs` (2240 lines) | COFF object emission, COMDAT extraction |
| `/home/free/code/milohax/jeff/src/util/split.rs` (lines 1625-1677) | COMDAT symbol marking in split pipeline |
| `/home/free/code/milohax/jeff/src/obj/mod.rs` (line 92) | `comdat_symbols: HashSet<String>` field |
| `/home/free/code/milohax/dc3-decomp/build.ninja` | Build system: 251 decomp + 1969 split objects |
| `/home/free/code/milohax/dc3-decomp/build/373307D9/link_test.rsp` | Split-only link (2223 objects) |
| `/home/free/code/milohax/dc3-decomp/docs/sessions/JEFF_LINK_LIMITATIONS.md` | Link error documentation |
| `/home/free/code/milohax/dc3-decomp/docs/STATUS.md` | Project status with 749 count |

### Key Commits in Jeff

| Commit | Description |
|--------|-------------|
| `cf01a80` | **Root cause**: Restricted COMDAT to code-only, breaking .rdata strings |
| `d330415` | CRT initializer splitting, .CRT section renaming |
| `7c79b2a` | Hardened COMDAT linking |
| `c445657` | Original COMDAT + `__unwind$` support |
| `8de8375` | REL24 addend fix for COMDAT offsets |

### Evidence: Hash Mismatch

Decomp-compiled `Char.obj` (`build/373307D9/src/system/char/Char.obj`):
```
??_C@_0BA@A@CharBonesObject?$AA@
??_C@_08A@CharBone?$AA@
??_C@_0BL@A@src?1system?2char?1CharBone?4h?$AA@
```

Split `Char.obj` (`build/373307D9/obj/system/char/Char.obj`):
```
??_C@_0BA@JPIJPPAL@CharBonesObject?$AA@
??_C@_08INHJBBLC@CharBone?$AA@
??_C@_0DF@HLEDMLGN@e?3?2lazer_build_gmc1?2system?2src?2c@
```

Same strings, different COMDAT hashes, therefore different symbol names, therefore
unresolved when one side replaces the other.
