# fork-regress shared bash helpers. Sourced by run.sh and scenarios/*.sh.
#
# Required env (exported by run.sh):
#   BIN            xenia-headless under test (absolute)
#   BIN_LABEL      short label recorded in provenance
#   BIN_XXH3       its xxh3
#   DEFAULTS_TOML  its compiled-in cvar defaults (fr.py defaults)
#   CONTENT        pinned content dir (build_content.sh)
#   HARNESS        tools/fork-regress
#   PY             a python >= 3.11 (/usr/bin/python3)

FR_PY="$HARNESS/lib/fr.py"

# Never leave cores from a Checked assert behind (systemd-coredump fills up).
ulimit -c 0

# ---- capability -------------------------------------------------------------
# has_cvar <name> : the binary under test defines this cvar.
has_cvar() { "$PY" "$FR_PY" has-cvars "$DEFAULTS_TOML" "$@" >/dev/null; }

# ---- one xenia run ----------------------------------------------------------
# xr_skip <run-dir> <reason> [kind] : record a run that did not launch.
#   kind "skipped" (default): the binary lacks a capability -> SKIPPED
#   kind "inconclusive": the HOST was not fit to run it (busy GPU) -> INCONCLUSIVE
xr_skip() {
  local rd="$1" reason="$2" kind="${3:-skipped}"
  mkdir -p "$rd"
  RD="$rd" REASON="$reason" KIND="$kind" "$PY" - <<'PY'
import json, os
rd = os.environ["RD"]
meta = {os.environ["KIND"]: os.environ["REASON"], "binary": os.environ["BIN"],
        "binary_xxh3": os.environ.get("BIN_XXH3"),
        "binary_label": os.environ.get("BIN_LABEL"),
        "defaults_toml": os.environ.get("DEFAULTS_TOML")}
json.dump(meta, open(os.path.join(rd, "run_meta.json"), "w"), indent=2)
PY
}

# Accumulated per-run state, reset by xr_begin.
XR_ARGS=(); XR_DROPPED=(); XR_INPUTS=()

xr_begin() { XR_ARGS=(); XR_DROPPED=(); XR_INPUTS=(); }
# xr_arg <--cvar=value>...   : always passed
xr_arg() { XR_ARGS+=("$@"); }
# xr_opt <name> <value>      : passed only if the binary knows the cvar
xr_opt() {
  if has_cvar "$1"; then XR_ARGS+=("--$1=$2"); else XR_DROPPED+=("$1"); fi
}
# xr_input <label> <path>    : sha256 recorded in provenance
xr_input() { XR_INPUTS+=("$1=$2"); }

# _sample_load <pid> <file> : 1-min loadavg + PSI cpu some avg10 every 5 s
_sample_load() {
  local pid="$1" f="$2" l1 l5 psi
  echo "epoch load1 load5 psi_cpu_some_avg10" > "$f"
  while kill -0 "$pid" 2>/dev/null; do
    read -r l1 l5 _ < /proc/loadavg
    psi="$(awk '/^some/{split($2,a,"="); print a[2]}' /proc/pressure/cpu 2>/dev/null)"
    echo "$(date +%s) $l1 $l5 ${psi:-0}" >> "$f"
    sleep 5
  done
}

FR_INTERRUPTED=0
_xr_on_interrupt() {
  FR_INTERRUPTED=1
  [ -n "${XR_XPID:-}" ] && kill -TERM "$XR_XPID" 2>/dev/null
  [ -n "${XR_DPID:-}" ] && kill -TERM "$XR_DPID" 2>/dev/null
  [ -n "${XR_RD:-}" ] && echo interrupted > "$XR_RD/interrupted"
}

# xr_run <run-dir> <timeout_s> <config-src>
#   Runs "$BIN" "${XR_ARGS[@]}" with a private storage root and a private COPY
#   of the pinned config, bounded by timeout(1). Writes run.log, load.tsv,
#   run_meta.json. If XR_DRIVER is set, runs "$XR_DRIVER <run-dir> <pid>" in the
#   background alongside (bounded by the same timeout) and records its rc.
xr_run() {
  local rd="$1" tmo="$2" cfg="$3"
  mkdir -p "$rd/storage"
  cp "$cfg" "$rd/config.toml"
  local -a argv=( "--storage_root=$rd/storage" "--config=$rd/config.toml" "${XR_ARGS[@]}" )
  printf '%s\0' "${argv[@]}" > "$rd/argv.bin"
  printf '%q ' "$BIN" "${argv[@]}" > "$rd/cmd.txt"; echo >> "$rd/cmd.txt"

  local start end rc drv_rc=""
  XR_XPID=""; XR_DPID=""; XR_RD="$rd"
  start=$(date +%s.%N)
  ( cd "$rd" && exec timeout -k 10 "$tmo" "$BIN" "${argv[@]}" ) > "$rd/run.log" 2>&1 &
  XR_XPID=$!
  # Kill only what we launched, on any interruption of the harness.
  trap _xr_on_interrupt INT TERM
  _sample_load "$XR_XPID" "$rd/load.tsv" &
  local spid=$!
  if [ -n "${XR_DRIVER:-}" ]; then
    ( exec timeout -k 5 "$tmo" "$XR_DRIVER" "$rd" "$XR_XPID" ) > "$rd/driver.log" 2>&1 &
    XR_DPID=$!
  fi
  wait "$XR_XPID"; rc=$?
  # wait returns early (>128) when a trapped signal arrives: reap for real.
  while kill -0 "$XR_XPID" 2>/dev/null; do wait "$XR_XPID"; rc=$?; done
  end=$(date +%s.%N)
  if [ -n "$XR_DPID" ]; then
    # The driver may still be polling a socket the process no longer serves.
    kill -TERM "$XR_DPID" 2>/dev/null
    wait "$XR_DPID"; drv_rc=$?
  fi
  kill "$spid" 2>/dev/null; wait "$spid" 2>/dev/null
  trap - INT TERM

  RD="$rd" RC="$rc" START="$start" END="$end" CFG="$cfg" DRV_RC="$drv_rc" \
  DROPPED="${XR_DROPPED[*]:-}" INPUTS="$(printf '%s\n' "${XR_INPUTS[@]:-}")" \
  "$PY" - <<'PY'
import json, os
rd = os.environ["RD"]
argv = [a for a in open(os.path.join(rd, "argv.bin"), "rb").read().decode().split("\0") if a]
inputs = {}
for line in os.environ.get("INPUTS", "").splitlines():
    if "=" in line:
        k, v = line.split("=", 1)
        inputs[k] = v
meta = {
    "binary": os.environ["BIN"], "binary_xxh3": os.environ.get("BIN_XXH3"),
    "binary_label": os.environ.get("BIN_LABEL"),
    "binary_git": json.loads(os.environ.get("BIN_GIT_JSON") or "{}"),
    "defaults_toml": os.environ.get("DEFAULTS_TOML"),
    "config": os.path.join(rd, "config.toml"), "config_src": os.environ["CFG"],
    "argv": argv, "rc": int(os.environ["RC"]),
    "start_epoch": float(os.environ["START"]),
    "wall_s": round(float(os.environ["END"]) - float(os.environ["START"]), 2),
    "driver_rc": int(os.environ["DRV_RC"]) if os.environ.get("DRV_RC") else None,
    "dropped_cvars": os.environ.get("DROPPED", "").split(),
    "inputs": inputs,
    "interrupted": os.path.exists(os.path.join(rd, "interrupted")),
}
meta.update(json.loads(os.environ.get("XR_META_EXTRA") or "{}"))
json.dump(meta, open(os.path.join(rd, "run_meta.json"), "w"), indent=2)
PY
  # Storage holds the title's content/cache (RB3 saves, shader caches): large
  # and never needed after analysis.
  [ "${FR_KEEP_STORAGE:-0}" = 1 ] || rm -rf "$rd/storage"
  # xenia also writes its own xenia-headless.log into the CWD; run.log (stdout
  # + stderr) is the superset the analyzers read.
  [ "${FR_KEEP_XLOG:-0}" = 1 ] || rm -f "$rd/xenia-headless.log"
  return 0
}

# Common DC3-original-layout cvars (the oracle command, docs/dc3-oracle).
# The resolver inputs are passed EXPLICITLY so no host path is auto-probed.
dc3_original_args() {
  local gpu="$1" timeout_ms="$2"
  local I="$CONTENT/dc3-inputs"
  xr_input xex "$CONTENT/dc3-original/debug.xex"
  xr_input ymca "$I/xenia-ymca.txt"
  xr_input symbols "$I/symbols.dc3-decomp-c362ede1c.txt"
  xr_input fingerprints "$I/dc3_nui_fingerprints.xenia-a5fc2f1b6.txt"
  xr_arg "--target=$CONTENT/dc3-original/debug.xex" "--gpu=$gpu" \
         --dc3_nui_patch_layout=original --dc3_crt_skip_nui=true \
         --stub_nui_functions=true --fake_kinect_data=true \
         "--scripted_input_file=$I/xenia-ymca.txt" \
         "--headless_timeout_ms=$timeout_ms"
  xr_opt dc3_nui_symbol_map_path "$I/symbols.dc3-decomp-c362ede1c.txt"
  xr_opt dc3_nui_layout_fingerprint_cache_path "$I/dc3_nui_fingerprints.xenia-a5fc2f1b6.txt"
  # Without this the early manifest load auto-probes
  # dc3-decomp/build/373307D9/xenia_dc3_patch_manifest.json (measured: it did,
  # on the Aug-29 binary). On the original layout it is then disabled by the
  # fingerprint mismatch, but the path must still be a pinned input.
  xr_input manifest "$CONTENT/dc3-decomp-2026-08-24/xenia_dc3_patch_manifest.json"
  xr_opt dc3_nui_patch_manifest_path "$CONTENT/dc3-decomp-2026-08-24/xenia_dc3_patch_manifest.json"
}

# Common RB3 clean-TU5 cvars (plan §4.2 S4).
rb3_tu5_args() {
  local timeout_ms="$1"
  xr_input xex "$CONTENT/rb3/tu5-clean-nodd/default.xex"
  xr_arg "--target=$CONTENT/rb3/tu5-clean-nodd/default.xex" --gpu=null \
         --protect_zero=false --break_on_debugbreak=false \
         "--headless_timeout_ms=$timeout_ms" \
         --rb3_tu5_app_run_direct=true --rb3_no_char_preview=true \
         --rb3dx_offline_join=true --rb3dx_skip_calibration=true \
         --rb3dx_autoconfirm_parts=true --rb3dx_ui_probe=true \
         --local_user_count=2 --scripted_pad_subtypes=1,8
  xr_opt rb3_stream_census true
  if [ -f "$CONTENT/rb3/mogg_key_table.hex" ] && has_cvar rb3_mogg_key_table; then
    xr_input mogg_key_table "$CONTENT/rb3/mogg_key_table.hex"
    XR_ARGS+=( "--rb3_mogg_key_table=$(tr -d ' \n' < "$CONTENT/rb3/mogg_key_table.hex")" )
  else
    XR_DROPPED+=( rb3_mogg_key_table )
  fi
}

# RB3DX: plan §4.2 S5. Plain A presses walk the whole flow (memory
# rb3dx-boots-to-gameplay): A at 8 s, then every 5 s.
rb3dx_args() {
  local timeout_ms="$1" s=8 seq=""
  while [ $s -le $(( timeout_ms / 1000 - 10 )) ]; do seq+="${s}s:A,"; s=$(( s + 5 )); done
  xr_input xex "$CONTENT/rb3/rb3dx/default.xex"
  xr_arg "--target=$CONTENT/rb3/rb3dx/default.xex" --gpu=null \
         --protect_zero=false --rb3dx_skip_calibration=true --rb3dx_ui_probe=true \
         --break_on_debugbreak=false "--headless_timeout_ms=$timeout_ms" \
         "--scripted_input=${seq%,}"
  xr_opt rb3_stream_census true
}
