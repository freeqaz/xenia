# Xenia DC3 Fork - Claude Context

Xenia fork with DC3 (Dance Central 3) boot hack pack for running the decomp build.

## Build

```bash
make -C build -f Makefile xenia-headless   # Checked config
```

## Run

```bash
build/bin/Linux/Checked/xenia-headless \
  --target=~/code/milohax/dc3-decomp/build/373307D9/default.xex \
  --dc3_nui_patch_layout=auto --dc3_crt_skip_nui=true \
  --headless_timeout_ms=120000
```

## DC3 Hack Pack

The main file is `src/xenia/dc3_hack_pack.cc`. It patches the decomp XEX at load time to work around missing subsystems (NUI/Kinect, Holmes debug networking, XAUDIO2, etc.).

### Address Resolution

Hardcoded guest addresses in `dc3_hack_pack.cc` come from the PE/MAP produced by the dc3-decomp linker. When the decomp is rebuilt (`ninja` in dc3-decomp), the PE layout changes and ALL hardcoded addresses become stale.

The **patch manifest** (`xenia_dc3_patch_manifest.json`) maps symbolic names to current addresses. `PatchStub8Resolved()` looks up names in the manifest first and only falls back to the hardcoded address if the name isn't found. Stubs registered in the manifest's `hack_pack_stubs` section auto-resolve correctly across rebuilds.

### After Rebuilding dc3-decomp

When the decomp PE changes (new functions matching, source changes, etc.), run these scripts **in dc3-decomp** to update the XEX, manifest, and symbol table:

```bash
cd ~/code/milohax/dc3-decomp

# 1. Rebuild the PE
ninja

# 2. Rebuild the XEX and regenerate the patch manifest
#    Uses the MAP file to resolve all hack_pack_stubs addresses.
#    Pass the runtime fingerprint (from Xenia logs) to enable fingerprint matching.
venv/bin/python scripts/build/build_xex.py \
  --pe build/373307D9/default.exe \
  --original-xex orig/373307D9/default.xex \
  --output build/373307D9/default.xex \
  --build-label decomp \
  --xenia-runtime-fnv1a64=<FINGERPRINT>

# 3. Update the symbol table (used by Xenia for NUI symbol resolution)
venv/bin/python scripts/extract_decomp_symbols.py --apply

# 4. (Optional) Regenerate the manifest separately
venv/bin/python scripts/build/generate_xenia_dc3_patch_manifest.py \
  --pe build/373307D9/default.exe \
  --map build/373307D9/default.map \
  --xex orig/373307D9/default.xex \
  --output build/373307D9/xenia_dc3_patch_manifest.json \
  --build-label decomp \
  --xenia-runtime-fnv1a64=<FINGERPRINT>
```

The runtime fingerprint is logged by Xenia on startup:
```
DC3: .text fingerprint addr=XXXXXXXX size=0xXXXXXXX fnv1a64=XXXXXXXXXXXXXXXX
```

The fingerprint file at `docs/dc3-boot/dc3_nui_fingerprints.txt` caches known fingerprints. The runtime fingerprint changes when XEX import patching modifies .text bytes (which happens even if the PE .text is identical).

### Adding New Stubs

When adding a new stub to `kDebugStubTable` in `dc3_hack_pack.cc`:
1. Add the mangled symbol name to `HACK_PACK_STUBS` in `dc3-decomp/scripts/build/generate_xenia_dc3_patch_manifest.py`
2. Regenerate the manifest (step 2 or 4 above)
3. The stub will then auto-resolve via `PatchStub8Resolved()` across future rebuilds

### Key Files

- `src/xenia/dc3_hack_pack.cc` — All DC3 boot patches
- `docs/dc3-boot/dc3_nui_fingerprints.txt` — Runtime .text fingerprint cache
- `docs/dc3-boot/STATUS.md` — Boot progression status
- `docs/dc3-boot/TODO.md` — Current work items
