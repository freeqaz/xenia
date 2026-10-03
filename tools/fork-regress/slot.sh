#!/bin/bash
# slot.sh <command...> -- run a fork-regress harness command in one of three
# shared slots, at most ONE slot per owner (lane), so lanes sharing a host
# interleave instead of starving each other.
#
# Slot 1 is $FR_LOCK_DIR/fork-regress.lock (the lock older docs flock
# directly, so those callers still exclude slot-1 users). Slots 2 and 3 are
# only taken while the 1-minute load is below FR_SLOT_LOAD_MAX (default 40,
# half the harness's INCONCLUSIVE gate of 80).
#
# Owner: $FR_SLOT_OWNER, else the first $FR_LOCK_DIR/xenia-<lane> path in the
# arguments, else the basename of $PWD.
# FR_SLOT_PRIORITY=1 rescans every 3 s instead of every 15 s.
# FR_LOCK_DIR defaults to /home/free/tmp, like run.sh's content dir.
#
# A slot is held for the whole command, so wrap one run.sh call (or use
# ab.sh --slot, which takes a slot per run) rather than a multi-run sequence.
set -u
LOCK_DIR="${FR_LOCK_DIR:-/home/free/tmp}"
LOAD_MAX="${FR_SLOT_LOAD_MAX:-40}"
SLOTS=("$LOCK_DIR/fork-regress.lock" "$LOCK_DIR/fork-regress.slot2.lock" "$LOCK_DIR/fork-regress.slot3.lock")
[ $# -ge 1 ] || { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
owner="${FR_SLOT_OWNER:-}"
if [ -z "$owner" ]; then
  owner=$(printf '%s\n' "$@" | grep -oE "$LOCK_DIR/xenia-[A-Za-z0-9_-]+" | head -1)
  owner="${owner##*/}"
fi
[ -z "$owner" ] && owner=$(basename "$PWD")
exec {ofd}>>"$LOCK_DIR/fr-owner-${owner}.lock"
flock "$ofd"   # one slot per owner: wait for this owner's previous command
while true; do
  for i in "${!SLOTS[@]}"; do
    if [ "$i" -gt 0 ]; then
      load=$(cut -d' ' -f1 /proc/loadavg)
      awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN{exit !(l < m)}' || continue
    fi
    exec {fd}>>"${SLOTS[$i]}"
    if flock -n "$fd"; then
      echo "fr-slot: owner $owner, slot $((i+1)) acquired (load $(cut -d' ' -f1 /proc/loadavg))" >&2
      "$@"
      exit $?
    fi
    exec {fd}>&-
  done
  [ "${FR_SLOT_PRIORITY:-0}" = 1 ] && sleep 3 || sleep 15
done
