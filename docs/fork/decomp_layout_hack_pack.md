# DC3 decomp-layout hack pack: rebuild workflow

Moved here from the old `CLAUDE.md` (2026-10-02). The pack is **kept** (cleanup
decision 2): it lives in `src/xenia/titles/dc3/decomp/`, is gated on the DC3
title plus a detected decomp layout, and is what harness scenario S3 (the
627 forced-trap fingerprint) boots. The long-term goal is to replace each hack
with a real Xenia fix ([DC3_HACK_GAP_ANALYSIS.md](cleanup/DC3_HACK_GAP_ANALYSIS.md)).

S3 runs on **pinned** inputs (`tools/fork-regress/build_content.sh`), so the
workflow below is only needed to boot a *fresh* decomp build by hand.

## Address resolution

Hardcoded guest addresses in `dc3_hack_pack.cc` come from the PE/MAP produced
by the dc3-decomp linker; every decomp rebuild moves them. The **patch
manifest** (`xenia_dc3_patch_manifest.json`, `--dc3_nui_patch_manifest_path`)
maps symbolic names to current addresses. `PatchStub8Resolved()` looks a name
up in the manifest first and falls back to the hardcoded address only when the
name is missing, so stubs listed in the manifest's `hack_pack_stubs` section
survive rebuilds.

## After rebuilding dc3-decomp

Run in dc3-decomp:

```bash
cd ~/code/milohax/dc3-decomp
ninja                                               # 1. rebuild the PE
venv/bin/python scripts/build/build_xex.py \
  --pe build/373307D9/default.exe \
  --original-xex orig/373307D9/default.xex \
  --output build/373307D9/default.xex \
  --build-label decomp \
  --xenia-runtime-fnv1a64=<FINGERPRINT>             # 2. XEX + patch manifest
venv/bin/python scripts/extract_decomp_symbols.py --apply   # 3. symbol table
```

`scripts/build/generate_xenia_dc3_patch_manifest.py` regenerates the manifest
on its own (same `--pe/--map/--xex/--output/--build-label/--xenia-runtime-fnv1a64`
arguments). The runtime fingerprint is logged at startup:

```
DC3: .text fingerprint addr=XXXXXXXX size=0xXXXXXXX fnv1a64=XXXXXXXXXXXXXXXX
```

`docs/dc3-boot/dc3_nui_fingerprints.txt` caches known fingerprints (the
resolver still auto-probes that path, which is why the file has not moved).
The fingerprint changes whenever XEX import patching touches `.text`, even if
the PE's `.text` is identical.

## Adding a stub

1. Add the entry to `kDebugStubTable` in `src/xenia/titles/dc3/decomp/dc3_hack_pack.cc`.
2. Add its mangled name to `HACK_PACK_STUBS` in
   `dc3-decomp/scripts/build/generate_xenia_dc3_patch_manifest.py`.
3. Regenerate the manifest; the stub then resolves through `PatchStub8Resolved()`.

Run (decomp build, by hand):

```bash
build/bin/Linux/Checked/xenia-headless \
  --target=~/code/milohax/dc3-decomp/build/373307D9/default.xex \
  --dc3_nui_patch_layout=auto --dc3_crt_skip_nui=true \
  --headless_timeout_ms=120000 --storage_root=$(mktemp -d)
```

Always pass a private `--storage_root`: without it Xenia rewrites the shared
`~/.local/share/Xenia/xenia.config.toml`.
