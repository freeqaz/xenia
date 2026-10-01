#!/bin/bash
# S2 -- S1 with the DC3 DTA evaluation channel on; lib/dta_driver.py sends the
# round-trip queries through dc3-decomp/tools/console/dc3_eval.py -T xenia.
# usage: S2.sh <run-dir> <run-index>
set -uo pipefail
source "$HARNESS/lib/common.sh"
RD="$1"
TIMEOUT_S="${FR_S1_TIMEOUT_S:-240}"
if ! has_cvar dc3_dta_channel; then
  xr_skip "$RD" "binary has no --dc3_dta_channel (predates the dc3-oracle channel)"
  exit 0
fi
xr_begin
dc3_original_args null $(( (TIMEOUT_S - 10) * 1000 ))
xr_arg "--dc3_dta_channel=$RD/dta.sock"
xr_input dc3_eval "${DC3_EVAL:-/home/free/code/milohax/dc3-decomp/tools/console/dc3_eval.py}"
mkdir -p "$RD"
cat > "$RD/driver.sh" <<DRV
#!/bin/bash
exec "$PY" "$HARNESS/lib/dta_driver.py" "\$@"
DRV
chmod +x "$RD/driver.sh"
XR_DRIVER="$RD/driver.sh" xr_run "$RD" "$TIMEOUT_S" "$HARNESS/config/dc3-oracle.defaults.toml"
