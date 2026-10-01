#!/bin/bash
# S4 -- RB3 clean retail TU5 (dirty-disc bypass) to gameplay entry; with the
# derived mogg key table, through song-stream playback.
# usage: S4.sh <run-dir> <run-index>
set -uo pipefail
source "$HARNESS/lib/common.sh"
RD="$1"
TIMEOUT_S="${FR_S4_TIMEOUT_S:-420}"
if ! has_cvar rb3_tu5_app_run_direct; then
  xr_skip "$RD" "binary has no --rb3_tu5_app_run_direct (predates the RB3 TU5 work)"
  exit 0
fi
xr_begin
rb3_tu5_args $(( (TIMEOUT_S - 20) * 1000 ))
# Seed the private content root with the profile content s66 started from.
SEED="$CONTENT/rb3/seed-post-s66-2026-08-29/content"
if [ "${FR_RB3_SEED:-1}" = 1 ] && [ -d "$SEED" ]; then
  mkdir -p "$RD/storage/content"
  cp -r "$SEED/." "$RD/storage/content/"
  chmod -R u+w "$RD/storage/content"
  xr_input seed_globaloptions "$SEED/45410914/00000001/globaloptions/globaloptions"
  xr_input seed_band3_save "$SEED/45410914/00000001/band3/save.dat"
fi
xr_run "$RD" "$TIMEOUT_S" "$HARNESS/config/shared-2026-08-29.toml"
