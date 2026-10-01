#!/bin/bash
# S3 -- DC3 decomp layout (2026-08-24 build), 120 s, count forced traps.
# Config: the pinned 2026-08-29 SHARED toml, which every 627 result was
# measured under. Resolver inputs explicit (no auto-probed host path).
# usage: S3.sh <run-dir> <run-index>
set -uo pipefail
source "$HARNESS/lib/common.sh"
RD="$1"
D="$CONTENT/dc3-decomp-2026-08-24"
xr_begin
xr_input xex "$D/default.xex"
xr_input manifest "$D/xenia_dc3_patch_manifest.json"
xr_input symbols "$D/symbols.dc3-decomp-3504a8a58.txt"
xr_input fingerprints "$D/dc3_nui_fingerprints.xenia-a5fc2f1b6.txt"
xr_arg "--target=$D/default.xex" --dc3_nui_patch_layout=auto --dc3_crt_skip_nui=true \
       --break_on_debugbreak=false --headless_timeout_ms=120000 --gpu=null
xr_opt dc3_nui_symbol_map_path "$D/symbols.dc3-decomp-3504a8a58.txt"
xr_opt dc3_nui_layout_fingerprint_cache_path "$D/dc3_nui_fingerprints.xenia-a5fc2f1b6.txt"
xr_opt dc3_nui_patch_manifest_path "$D/xenia_dc3_patch_manifest.json"
xr_run "$RD" 140 "$HARNESS/config/shared-2026-08-29.toml"
