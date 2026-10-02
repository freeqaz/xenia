#!/usr/bin/env bash
set -euo pipefail

XENIA_DIR="${XENIA_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
BIN="${XENIA_BIN:-$XENIA_DIR/build/bin/Linux/Debug/xenia-headless}"

BRIDGE_XEX="${DC3_RSP_BRIDGE_XEX:-/home/free/code/milohax/dc3-decomp/build/373307D9/default.xex}"
BRIDGE_LOG="${DC3_RSP_BRIDGE_LOG:-}"
BRIDGE_SNAPSHOT_JSON="${DC3_RSP_BRIDGE_SNAPSHOT_JSON:-}"
BRIDGE_TIMEOUT_SECS="${DC3_RSP_BRIDGE_TIMEOUT_SECS:-20}"
BRIDGE_HEADLESS_TIMEOUT_MS="${DC3_RSP_BRIDGE_HEADLESS_TIMEOUT_MS:-10000}"
BRIDGE_TMPDIR="${DC3_RSP_BRIDGE_TMPDIR:-/tmp/xenia_dc3_rsp_snapshot_bridge}"
BRIDGE_HOST="${DC3_RSP_BRIDGE_HOST:-127.0.0.1}"
BRIDGE_PORT="${DC3_RSP_BRIDGE_PORT:-9001}"
BRIDGE_GDB_BIN="${DC3_RSP_BRIDGE_GDB_BIN:-powerpc-none-eabi-gdb}"
BRIDGE_AUTO_GDB="${DC3_RSP_BRIDGE_AUTO_GDB:-0}"
BRIDGE_GDB_CMD_FILE="${DC3_RSP_BRIDGE_GDB_CMD_FILE:-$BRIDGE_TMPDIR/attach.gdb}"
BRIDGE_CAPTURE_SNAPSHOT_JSON="${DC3_RSP_BRIDGE_CAPTURE_SNAPSHOT_JSON:-0}"
BRIDGE_PACKET_TRACE="${DC3_RSP_BRIDGE_PACKET_TRACE:-$BRIDGE_TMPDIR/rsp_packets.log}"

BRIDGE_SYMBOLS="${DC3_RSP_BRIDGE_SYMBOLS:-/home/free/code/milohax/dc3-decomp/config/373307D9/symbols.txt}"
GUEST_DISASM_TOOL="${DC3_RSP_BRIDGE_GUEST_DISASM_TOOL:-$XENIA_DIR/tools/dc3_guest_disasm.py}"
CRASH_TRIAGE_TOOL="${DC3_RSP_BRIDGE_CRASH_TRIAGE_TOOL:-$XENIA_DIR/tools/dc3_crash_signature_triage.py}"
RSP_MOCK_TOOL="${DC3_RSP_BRIDGE_RSP_MOCK_TOOL:-$XENIA_DIR/tools/dc3_gdb_rsp_mvp_mock.py}"

ENABLE_TELEMETRY="${DC3_RSP_BRIDGE_ENABLE_TELEMETRY:-1}"
GPU_MODE="${DC3_RSP_BRIDGE_GPU:-null}"
BREAK_ON_UNIMPL="${DC3_RSP_BRIDGE_BREAK_ON_UNIMPL:-0}"

normalize_bool() {
  case "${1,,}" in
    1|true|yes|on) echo "true" ;;
    0|false|no|off) echo "false" ;;
    *)
      echo "error: invalid boolean value '$1' (expected 0/1/true/false)" >&2
      return 1
      ;;
  esac
}

ENABLE_TELEMETRY_BOOL="$(normalize_bool "$ENABLE_TELEMETRY")" || exit 2
BREAK_ON_UNIMPL_BOOL="$(normalize_bool "$BREAK_ON_UNIMPL")" || exit 2
BRIDGE_AUTO_GDB_BOOL="$(normalize_bool "$BRIDGE_AUTO_GDB")" || exit 2
BRIDGE_CAPTURE_SNAPSHOT_JSON_BOOL="$(normalize_bool "$BRIDGE_CAPTURE_SNAPSHOT_JSON")" || exit 2

mkdir -p "$BRIDGE_TMPDIR"

RUN_LOG="$BRIDGE_TMPDIR/run.log"
RUN_TELEMETRY="$BRIDGE_TMPDIR/runtime_telemetry.jsonl"
RUN_SNAPSHOT_JSON="$BRIDGE_TMPDIR/crash_snapshot.json"
CRASH_TRIAGE_TXT="$BRIDGE_TMPDIR/crash_triage.txt"
CRASH_TRIAGE_JSON="$BRIDGE_TMPDIR/crash_triage.json"
CRASH_DISASM_TXT="$BRIDGE_TMPDIR/crash_disasm.txt"

detect_gdb() {
  local requested="$1"
  if command -v "$requested" >/dev/null 2>&1; then
    command -v "$requested"
    return 0
  fi
  if command -v gdb-multiarch >/dev/null 2>&1; then
    command -v gdb-multiarch
    return 0
  fi
  if command -v gdb >/dev/null 2>&1; then
    command -v gdb
    return 0
  fi
  echo ""
}

ensure_crash_log() {
  if [[ -n "$BRIDGE_SNAPSHOT_JSON" ]]; then
    if [[ ! -f "$BRIDGE_SNAPSHOT_JSON" ]]; then
      echo "error: DC3_RSP_BRIDGE_SNAPSHOT_JSON not found: $BRIDGE_SNAPSHOT_JSON" >&2
      return 1
    fi
    # The RSP mock can consume the JSON directly, but triage/disasm helpers still use a log path.
    if [[ -n "$BRIDGE_LOG" && -f "$BRIDGE_LOG" ]]; then
      echo "$BRIDGE_LOG"
      return 0
    fi
    echo ""
    return 0
  fi
  if [[ -n "$BRIDGE_LOG" ]]; then
    if [[ ! -f "$BRIDGE_LOG" ]]; then
      echo "error: DC3_RSP_BRIDGE_LOG not found: $BRIDGE_LOG" >&2
      return 1
    fi
    echo "$BRIDGE_LOG"
    return 0
  fi

  if [[ ! -x "$BIN" ]]; then
    echo "error: xenia-headless not found at $BIN" >&2
    return 1
  fi
  if [[ ! -f "$BRIDGE_XEX" ]]; then
    echo "error: XEX not found: $BRIDGE_XEX" >&2
    return 1
  fi

  rm -f "$RUN_LOG" "$RUN_TELEMETRY"
  rm -f "$RUN_SNAPSHOT_JSON"
  local -a args=(
    --gpu="$GPU_MODE"
    --target="$BRIDGE_XEX"
    --headless_timeout_ms="$BRIDGE_HEADLESS_TIMEOUT_MS"
    --break_on_unimplemented_instructions="$BREAK_ON_UNIMPL_BOOL"
  )
  if [[ "$ENABLE_TELEMETRY_BOOL" == "true" ]]; then
    args+=(--dc3_runtime_telemetry_enable=true --dc3_runtime_telemetry_path="$RUN_TELEMETRY")
  fi
  if [[ "$BRIDGE_CAPTURE_SNAPSHOT_JSON_BOOL" == "true" ]]; then
    args+=(--dc3_crash_snapshot_path="$RUN_SNAPSHOT_JSON")
  fi

  echo "== Capturing crash snapshot source run =="
  echo "BIN: $BIN"
  echo "XEX: $BRIDGE_XEX"
  echo "LOG: $RUN_LOG"
  set +e
  timeout "${BRIDGE_TIMEOUT_SECS}s" "$BIN" "${args[@]}" >"$RUN_LOG" 2>&1
  local rc=$?
  set -e
  echo "Run rc=$rc"
  if [[ "$BRIDGE_CAPTURE_SNAPSHOT_JSON_BOOL" == "true" && -f "$RUN_SNAPSHOT_JSON" && -s "$RUN_SNAPSHOT_JSON" ]]; then
    BRIDGE_SNAPSHOT_JSON="$RUN_SNAPSHOT_JSON"
    echo "Snapshot JSON captured: $RUN_SNAPSHOT_JSON"
  fi
  echo "$RUN_LOG"
}

LOG_PATH="$(ensure_crash_log)"

if [[ ! -f "$RSP_MOCK_TOOL" ]]; then
  echo "error: RSP mock tool not found: $RSP_MOCK_TOOL" >&2
  exit 1
fi

if [[ -n "$LOG_PATH" && -f "$CRASH_TRIAGE_TOOL" ]]; then
  triage_args=(--log "$LOG_PATH" --allow-no-crash --json-out "$CRASH_TRIAGE_JSON" --image "$BRIDGE_XEX")
  if [[ -n "$BRIDGE_SYMBOLS" && -f "$BRIDGE_SYMBOLS" ]]; then
    triage_args+=(--symbols "$BRIDGE_SYMBOLS")
  fi
  python3 "$CRASH_TRIAGE_TOOL" "${triage_args[@]}" >"$CRASH_TRIAGE_TXT" 2>&1 || true
  [[ -f "$CRASH_TRIAGE_TXT" ]] && echo "Crash triage: $CRASH_TRIAGE_TXT"
fi

if [[ -n "$LOG_PATH" && -f "$GUEST_DISASM_TOOL" ]]; then
  disasm_args=(--image "$BRIDGE_XEX" --xenia-log "$LOG_PATH")
  if [[ -n "$BRIDGE_SYMBOLS" && -f "$BRIDGE_SYMBOLS" ]]; then
    disasm_args+=(--symbols "$BRIDGE_SYMBOLS")
  fi
  python3 "$GUEST_DISASM_TOOL" "${disasm_args[@]}" >"$CRASH_DISASM_TXT" 2>&1 || true
  [[ -f "$CRASH_DISASM_TXT" ]] && echo "Crash disasm: $CRASH_DISASM_TXT"
fi

if [[ -n "$BRIDGE_SNAPSHOT_JSON" ]]; then
  if [[ ! -f "$BRIDGE_SNAPSHOT_JSON" ]]; then
    echo "error: snapshot JSON not found: $BRIDGE_SNAPSHOT_JSON" >&2
    exit 1
  fi
elif [[ -n "$LOG_PATH" ]]; then
  if ! rg -q "==== CRASH DUMP ====" "$LOG_PATH"; then
    echo "error: no crash dump found in $LOG_PATH (cannot build RSP snapshot server)" >&2
    exit 1
  fi
else
  echo "error: no crash log or snapshot JSON available for RSP mock" >&2
  exit 1
fi

GDB_BIN_PATH="$(detect_gdb "$BRIDGE_GDB_BIN")"
PE_SYMBOL_FILE=""
if [[ -f "${BRIDGE_XEX%.xex}.exe" ]]; then
  PE_SYMBOL_FILE="${BRIDGE_XEX%.xex}.exe"
fi

write_gdb_cmd_file() {
  local path="$1"
  local host="$2"
  local port="$3"
  local pe_file="$4"
  mkdir -p "$(dirname "$path")"
  {
    echo "set pagination off"
    echo "set confirm off"
    if [[ -n "$pe_file" ]]; then
      echo "symbol-file $pe_file"
    fi
    echo "target remote $host:$port"
    echo "# Suggested first commands:"
    echo "# info registers pc lr ctr r1"
    echo "# x/8wx \$pc-0x10"
    echo "# x/8i \$pc-0x10"
  } >"$path"
}

write_gdb_cmd_file "$BRIDGE_GDB_CMD_FILE" "$BRIDGE_HOST" "$BRIDGE_PORT" "$PE_SYMBOL_FILE"

RSP_SOURCE_ARGS=()
if [[ -n "$BRIDGE_SNAPSHOT_JSON" ]]; then
  RSP_SOURCE_ARGS+=(--snapshot-json "$BRIDGE_SNAPSHOT_JSON")
else
  RSP_SOURCE_ARGS+=(--log "$LOG_PATH")
fi

echo
echo "== Snapshot RSP Mock =="
if [[ -n "$BRIDGE_SNAPSHOT_JSON" ]]; then
  echo "Snapshot JSON: $BRIDGE_SNAPSHOT_JSON"
fi
if [[ -n "$LOG_PATH" ]]; then
  echo "Log:   $LOG_PATH"
fi
echo "Image: $BRIDGE_XEX"
echo "Listen: $BRIDGE_HOST:$BRIDGE_PORT"
if [[ -n "$GDB_BIN_PATH" ]]; then
  echo "Attach command:"
  echo "  $GDB_BIN_PATH -ex 'target remote $BRIDGE_HOST:$BRIDGE_PORT'"
  echo "Command file:"
  echo "  $GDB_BIN_PATH -x $BRIDGE_GDB_CMD_FILE"
  echo "Optional symbols (if using PE image):"
  if [[ -n "$PE_SYMBOL_FILE" ]]; then
    echo "  (auto) symbol-file $PE_SYMBOL_FILE"
  else
    echo "  (gdb) symbol-file /path/to/default.exe"
  fi
else
  echo "GDB not found in PATH (looked for $BRIDGE_GDB_BIN, gdb-multiarch, gdb)"
fi
echo "GDB command file: $BRIDGE_GDB_CMD_FILE"
echo "RSP packet trace: $BRIDGE_PACKET_TRACE"
echo
echo "Starting RSP mock server (Ctrl-C to stop)..."

if [[ "$BRIDGE_AUTO_GDB_BOOL" == "true" && -n "$GDB_BIN_PATH" ]]; then
  python3 "$RSP_MOCK_TOOL" \
    "${RSP_SOURCE_ARGS[@]}" \
    --packet-trace "$BRIDGE_PACKET_TRACE" \
    --image "$BRIDGE_XEX" \
    --host "$BRIDGE_HOST" \
    --port "$BRIDGE_PORT" &
  RSP_PID=$!
  trap 'kill $RSP_PID 2>/dev/null || true' EXIT
  # Give the listener a moment to come up.
  sleep 0.2
  exec "$GDB_BIN_PATH" -x "$BRIDGE_GDB_CMD_FILE"
fi

exec python3 "$RSP_MOCK_TOOL" \
  "${RSP_SOURCE_ARGS[@]}" \
  --packet-trace "$BRIDGE_PACKET_TRACE" \
  --image "$BRIDGE_XEX" \
  --host "$BRIDGE_HOST" \
  --port "$BRIDGE_PORT"
