#!/bin/bash
# S1 -- DC3 original debug.xex, null GPU, ymca flow (the oracle boot).
# usage: S1.sh <run-dir> <run-index>     (env from run.sh)
set -uo pipefail
source "$HARNESS/lib/common.sh"
RD="$1"
TIMEOUT_S="${FR_S1_TIMEOUT_S:-240}"
xr_begin
dc3_original_args null $(( (TIMEOUT_S - 10) * 1000 ))
xr_run "$RD" "$TIMEOUT_S" "$HARNESS/config/dc3-oracle.defaults.toml"
