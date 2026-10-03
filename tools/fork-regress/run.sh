#!/bin/bash
# fork-regress -- regression harness for the milohax Xenia fork (DC3 + RB3).
#
# usage: tools/fork-regress/run.sh <xenia-headless> <out-dir> [options]
#
#   --scenarios S0,S1,...   default: S0,S1,S1V,S2,S3,S4,S5,S6
#   --repeat N              runs per scenario (default: the scenario's own N,
#                           see lib/fr.py SCENARIOS; S6 is always its 3 variants)
#   --retry K               re-run an INCONCLUSIVE run up to K more times (default 1)
#   --wait-load SECS        before each run, wait up to SECS for the 1-min load
#                           to fall to the gate (FR_LOAD_MAX); 0 = don't wait
#                           (default 0). Waiting is bounded; a run that starts
#                           loaded is still judged by the gate.
#   --label NAME            label recorded in provenance (default: binary basename)
#   --binary-rev SHA        git rev the binary was built from, when the binary
#                           does not live in a worktree's build/ (pinned copies)
#   --runs i,j              run only these run indices (others already in
#                           <out-dir> are kept and aggregated); used by ab.sh
#   --content DIR           pinned content (default $FORK_REGRESS_CONTENT or
#                           /home/free/tmp/fork-regress-content; build_content.sh)
#   --extra-arg ARG         append ARG (one --cvar=value) to every xenia command
#                           line of this invocation; repeatable. For same-binary
#                           A/Bs of one switch (e.g. --dc3_disable_hacks=X): an
#                           unknown cvar makes xenia print help and exit 0, so
#                           only pass cvars the binary defines.
#
# Output: <out-dir>/<scenario>/run-NN[.retryK]/{run.log,verdict.json,...},
#         <out-dir>/<scenario>/scenario.json, <out-dir>/summary.json.
# Compare against a baseline: tools/fork-regress/compare.py <baseline.json> <out-dir>/summary.json
#
# Every run: private --storage_root (deleted after analysis unless
# FR_KEEP_STORAGE=1), a private COPY of a pinned --config, every non-default
# cvar on the command line, bounded by timeout(1). Runs are sequential on
# purpose: parallel runs load the host, and the flow scenarios are load
# sensitive. Only processes this script launched are ever killed.
set -uo pipefail

HARNESS="$(cd "$(dirname "$0")" && pwd)"
export HARNESS
export PY="${PY:-/usr/bin/python3}"
"$PY" -c 'import tomllib' 2>/dev/null || { echo "need python >= 3.11 at PY=$PY" >&2; exit 2; }

[ $# -ge 2 ] || { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
BIN_ARG="$1"; OUT="$2"; shift 2
SCENARIOS="S0,S1,S1V,S2,S3,S4,S5,S6"
REPEAT=""; RUNS=""; RETRY=1; WAIT_LOAD=0; LABEL=""; BIN_REV=""; EXTRA_ARGS=()
CONTENT="${FORK_REGRESS_CONTENT:-/home/free/tmp/fork-regress-content}"
while [ $# -gt 0 ]; do
  case "$1" in
    --scenarios) SCENARIOS="$2"; shift 2 ;;
    --scenarios=*) SCENARIOS="${1#*=}"; shift ;;
    --repeat) REPEAT="$2"; shift 2 ;;
    --retry) RETRY="$2"; shift 2 ;;
    --runs) RUNS="$2"; shift 2 ;;
    --wait-load) WAIT_LOAD="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    --binary-rev) BIN_REV="$2"; shift 2 ;;
    --content) CONTENT="$2"; shift 2 ;;
    --extra-arg) EXTRA_ARGS+=("$2"); shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

BIN="$(readlink -f "$BIN_ARG")"
[ -x "$BIN" ] || { echo "not executable: $BIN_ARG" >&2; exit 2; }
[ -f "$CONTENT/MANIFEST.sha256" ] || { echo "no content at $CONTENT -- run build_content.sh" >&2; exit 2; }
mkdir -p "$OUT"; OUT="$(readlink -f "$OUT")"
export BIN CONTENT
# Newline-separated; xr_run appends each line as one argv entry.
FR_EXTRA_ARGS="$(printf '%s\n' "${EXTRA_ARGS[@]}")"
export FR_EXTRA_ARGS
export BIN_LABEL="${LABEL:-$(basename "$BIN")}"
export BIN_XXH3="$(xxhsum -H3 "$BIN" | awk '{print $1}' | sed 's/^XXH3_//')"

# Where did the binary come from? A build/bin/... inside a git worktree names
# its own tree; a pinned copy needs --binary-rev.
BIN_GIT_JSON="$(BIN="$BIN" REV="$BIN_REV" "$PY" - <<'PY'
import json, os, subprocess
b, rev = os.environ["BIN"], os.environ["REV"]
out = {}
if rev:
    out = {"head": rev, "source": "--binary-rev"}
else:
    d = os.path.dirname(b)
    try:
        top = subprocess.run(["git", "-C", d, "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, check=True).stdout.strip()
        if "/build/bin/" in b:
            head = subprocess.run(["git", "-C", top, "rev-parse", "HEAD"],
                                  capture_output=True, text=True).stdout.strip()
            dirty = subprocess.run(["git", "-C", top, "status", "--porcelain",
                                    "--ignore-submodules=all"],
                                   capture_output=True, text=True).stdout
            out = {"tree": top, "head": head,
                   "dirty_files": len([l for l in dirty.splitlines() if l.strip()]),
                   "source": "worktree of the binary (the tree's CURRENT HEAD; the "
                             "binary may be older than it)"}
    except subprocess.CalledProcessError:
        pass
print(json.dumps(out))
PY
)"
export BIN_GIT_JSON

echo "fork-regress: $BIN_LABEL xxh3=$BIN_XXH3 -> $OUT"
export DEFAULTS_TOML="$("$PY" "$HARNESS/lib/fr.py" defaults "$BIN" "$OUT/_cache")" || exit 1

OUT="$OUT" SCEN="$SCENARIOS" ARGS="$*" "$PY" - <<'PY'
import json, os, socket, time
json.dump({"binary": os.environ["BIN"], "binary_xxh3": os.environ["BIN_XXH3"],
           "binary_label": os.environ["BIN_LABEL"],
           "binary_git": json.loads(os.environ["BIN_GIT_JSON"] or "{}"),
           "scenarios": os.environ["SCEN"], "content": os.environ["CONTENT"],
           "load_max": float(os.environ.get("FR_LOAD_MAX") or 0) or None,
           "host": socket.gethostname(), "started": time.strftime("%Y-%m-%dT%H:%M:%S%z")},
          open(os.path.join(os.environ["OUT"], "invocation.json"), "w"), indent=2)
PY

load_gate() { "$PY" -c 'import sys,os; sys.path.insert(0, os.environ["HARNESS"]+"/lib"); import fr; print(fr.load_max())'; }
GATE="$(load_gate)"
wait_for_load() {
  [ "$WAIT_LOAD" -gt 0 ] || return 0
  local deadline=$(( $(date +%s) + WAIT_LOAD )) l1
  while :; do
    read -r l1 _ < /proc/loadavg
    awk -v a="$l1" -v b="$GATE" 'BEGIN{exit !(a<=b)}' && return 0
    [ "$(date +%s)" -ge "$deadline" ] && { echo "   load $l1 still > $GATE after ${WAIT_LOAD}s; running anyway"; return 0; }
    sleep 15
  done
}

scen_n() { "$PY" -c "import sys; sys.path.insert(0, '$HARNESS/lib'); import fr; print(fr.SCENARIOS['$1']['N'])"; }

INTERRUPTED=0
trap 'INTERRUPTED=1' INT TERM
IFS=',' read -r -a LIST <<< "$SCENARIOS"
for S in "${LIST[@]}"; do
  [ -x "$HARNESS/scenarios/$S.sh" ] || { echo "unknown scenario $S" >&2; continue; }
  N="$(scen_n "$S")"
  if [ -n "$REPEAT" ] && [ "$S" != S6 ] && [ "$S" != S0 ]; then N="$REPEAT"; fi
  echo "== $S x$N${RUNS:+ (runs $RUNS)}"
  mkdir -p "$OUT/$S"
  IDXS="$(seq 1 "$N")"
  [ -n "$RUNS" ] && IDXS="$(echo "$RUNS" | tr ',' ' ')"
  for i in $IDXS; do
    idx="$(printf 'run-%02d' "$i")"
    for a in $(seq 0 "$RETRY"); do
      [ "$INTERRUPTED" = 1 ] && break 3
      rd="$OUT/$S/$idx"; [ "$a" -gt 0 ] && rd="$rd.retry$a"
      # A fresh attempt 0 supersedes every earlier attempt of this index.
      [ "$a" -eq 0 ] && rm -rf "$OUT/$S/$idx".retry*
      rm -rf "$rd"
      [ "$S" != S0 ] && wait_for_load
      "$HARNESS/scenarios/$S.sh" "$rd" "$i"
      "$PY" "$HARNESS/lib/fr.py" finalize "$S" "$rd" | sed 's/^/   /'
      v="$("$PY" "$HARNESS/lib/fr.py" verdict-of "$rd")"
      # Optional per-scenario step that needs the verdict (S1V: write back
      # its warm pipeline cache only after a PASS).
      if [ -x "$HARNESS/scenarios/$S.post.sh" ]; then
        "$HARNESS/scenarios/$S.post.sh" "$rd" "$v" | sed 's/^/   /'
      fi
      [ "$v" = INCONCLUSIVE ] || break
    done
  done
  "$PY" "$HARNESS/lib/fr.py" aggregate "$S" "$OUT/$S"
done
echo "== summary ($OUT/summary.json)"
"$PY" "$HARNESS/lib/fr.py" summary "$OUT"
[ "$INTERRUPTED" = 1 ] && { echo "interrupted"; exit 130; }
exit 0
