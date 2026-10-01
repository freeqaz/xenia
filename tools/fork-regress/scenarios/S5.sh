#!/bin/bash
# S5 -- RB3DX (Rock Band 3 Deluxe) title -> menus -> gameplay, plain A presses.
# usage: S5.sh <run-dir> <run-index>
set -uo pipefail
source "$HARNESS/lib/common.sh"
RD="$1"
TIMEOUT_S="${FR_S5_TIMEOUT_S:-380}"
xr_begin
rb3dx_args $(( (TIMEOUT_S - 20) * 1000 ))
SEED="$CONTENT/rb3/seed-post-s66-2026-08-29/content"
if [ "${FR_RB3_SEED:-1}" = 1 ] && [ -d "$SEED" ]; then
  mkdir -p "$RD/storage/content"
  cp -r "$SEED/." "$RD/storage/content/"
  chmod -R u+w "$RD/storage/content"
fi
xr_run "$RD" "$TIMEOUT_S" "$HARNESS/config/shared-2026-08-29.toml"
