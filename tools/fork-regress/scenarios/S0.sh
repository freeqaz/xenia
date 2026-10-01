#!/bin/bash
# S0 -- static checks, no game: the binary's compiled-in cvar defaults (the
# comparator diffs them against the baseline binary's), and source ratchets on
# FR_SRC (default: this harness's own tree). Building the binaries and running
# the unit-test targets is left to the lane's own build step.
# usage: S0.sh <run-dir> <run-index>
set -uo pipefail
source "$HARNESS/lib/common.sh"
RD="$1"
mkdir -p "$RD"
SRC="${FR_SRC:-$(git -C "$HARNESS" rev-parse --show-toplevel)}"
RD="$RD" SRC="$SRC" "$PY" - <<'PY'
import json, os
meta = {"static": True, "binary": os.environ["BIN"], "binary_xxh3": os.environ.get("BIN_XXH3"),
        "binary_label": os.environ.get("BIN_LABEL"),
        "binary_git": json.loads(os.environ.get("BIN_GIT_JSON") or "{}"),
        "defaults_toml": os.environ["DEFAULTS_TOML"], "src_tree": os.environ["SRC"], "rc": 0}
json.dump(meta, open(os.path.join(os.environ["RD"], "run_meta.json"), "w"), indent=2)
PY
