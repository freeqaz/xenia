#!/bin/bash
# S6 -- cross-title inertness. Run index selects the variant:
#   1 DC1 TU0 60 s, no title cvars
#   2 DC3 ymca flow with every RB3 cvar the binary has turned on
#   3 RB3DX flow with every DC3 cvar the binary has turned on
# usage: S6.sh <run-dir> <run-index>
set -uo pipefail
source "$HARNESS/lib/common.sh"
RD="$1"; V="$2"
xr_begin
case "$V" in
  1)
    xr_input xex "$CONTENT/dc1/default.xex"
    xr_arg "--target=$CONTENT/dc1/default.xex" --gpu=null --break_on_debugbreak=false \
           --headless_timeout_ms=60000
    XR_META_EXTRA='{"variant": 1}' xr_run "$RD" 75 "$HARNESS/config/dc3-oracle.defaults.toml"
    ;;
  2)
    dc3_original_args null 170000
    for c in rb3_tu5_app_run_direct rb3_no_char_preview rb3dx_offline_join \
             rb3dx_skip_calibration rb3dx_autoconfirm_parts rb3dx_ui_probe \
             rb3_stream_census si_hook_verify; do
      xr_opt "$c" true
    done
    if [ -f "$CONTENT/rb3/mogg_key_table.hex" ] && has_cvar rb3_mogg_key_table; then
      XR_ARGS+=( "--rb3_mogg_key_table=$(tr -d ' \n' < "$CONTENT/rb3/mogg_key_table.hex")" )
    fi
    XR_META_EXTRA='{"variant": 2}' xr_run "$RD" 180 "$HARNESS/config/dc3-oracle.defaults.toml"
    ;;
  3)
    rb3dx_args 170000
    for c in fake_kinect_data stub_nui_functions dc3_crt_skip_nui dc3_ik_telemetry \
             dc3_gameplay_probe dc3_runtime_telemetry_enable dc3_headless_autonav; do
      xr_opt "$c" true
    done
    xr_opt dc3_nui_patch_layout original
    xr_opt dc3_runtime_telemetry_path "$RD/dc3_telemetry.jsonl"
    xr_opt dc3_dta_channel "$RD/dta.sock"
    XR_META_EXTRA='{"variant": 3}' xr_run "$RD" 180 "$HARNESS/config/shared-2026-08-29.toml"
    ;;
  *) echo "S6: unknown variant $V" >&2; exit 2 ;;
esac
