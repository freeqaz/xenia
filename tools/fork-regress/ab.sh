#!/bin/bash
# ab.sh -- interleave two binaries run by run, so host-load drift lands on both
# sides equally (plan §4.1). Typical: candidate vs reference.
#
# usage: tools/fork-regress/ab.sh <binA> <outA> <binB> <outB> [--scenarios S1,S3] [--repeat N]
#                                [--label-a L --label-b L --rev-a SHA --rev-b SHA]
#                                [--lock FILE]  (flock FILE around EACH run.sh call,
#                                 so lanes sharing a host interleave run by run)
#                                [--extra-a ARG --extra-b ARG]  (repeatable; run.sh
#                                 --extra-arg for one side only, e.g. a same-binary
#                                 A/B of --dc3_disable_hacks=<id>)
#                                [any other run.sh option, applied to both]
# Then: compare.py <(compare.py --make-baseline outB) outA, or against a
# checked-in baseline.
set -uo pipefail
HARNESS="$(cd "$(dirname "$0")" && pwd)"
PY="${PY:-/usr/bin/python3}"
[ $# -ge 4 ] || { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
A="$1"; OA="$2"; B="$3"; OB="$4"; shift 4
SCEN="S0,S1,S1V,S2,S3,S4,S5,S6"; REPEAT=""; REST=(); XA=(); XB=(); LOCK=()
while [ $# -gt 0 ]; do
  case "$1" in
    --scenarios) SCEN="$2"; shift 2 ;;
    --repeat) REPEAT="$2"; shift 2 ;;
    --label-a) XA+=(--label "$2"); shift 2 ;;
    --label-b) XB+=(--label "$2"); shift 2 ;;
    --rev-a) XA+=(--binary-rev "$2"); shift 2 ;;
    --rev-b) XB+=(--binary-rev "$2"); shift 2 ;;
    --lock) LOCK=(flock "$2"); shift 2 ;;
    --extra-a) XA+=(--extra-arg "$2"); shift 2 ;;
    --extra-b) XB+=(--extra-arg "$2"); shift 2 ;;
    *) REST+=("$1"); shift ;;
  esac
done
trap 'exit 130' INT TERM
IFS=',' read -r -a LIST <<< "$SCEN"
for S in "${LIST[@]}"; do
  N="$("$PY" -c "import sys; sys.path.insert(0, '$HARNESS/lib'); import fr; print(fr.SCENARIOS['$S']['N'])")"
  if [ -n "$REPEAT" ] && [ "$S" != S6 ] && [ "$S" != S0 ]; then N="$REPEAT"; fi
  for i in $(seq 1 "$N"); do
    "${LOCK[@]}" "$HARNESS/run.sh" "$A" "$OA" --scenarios "$S" --runs "$i" "${XA[@]}" "${REST[@]}" || exit $?
    "${LOCK[@]}" "$HARNESS/run.sh" "$B" "$OB" --scenarios "$S" --runs "$i" "${XB[@]}" "${REST[@]}" || exit $?
  done
done
