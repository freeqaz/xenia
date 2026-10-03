#!/bin/bash
# S1V.post.sh -- after finalize: refresh the persistent warm pipeline cache.
# usage: S1V.post.sh <run-dir> <verdict>
# Only a PASS writes back, and only a cache at least as large as the seed (the
# cache is load + append, so a smaller one means a truncated or foreign file).
# Copy to a temp file beside the seed, then rename: a concurrent run copying
# the seed sees the old file or the new one, never a partial one.
set -uo pipefail
source "$HARNESS/lib/common.sh"
RD="$1"; V="$2"
SEED="$(s1v_pcache_seed)"
NEW="$RD/pcache/$(basename "$SEED")"
[ "$V" = PASS ] || exit 0
[ -s "$NEW" ] || { echo "S1V pcache: run wrote no pipeline cache; seed unchanged"; exit 0; }
old=0; [ -f "$SEED" ] && old=$(stat -c %s "$SEED")
new=$(stat -c %s "$NEW")
if [ "$new" -lt "$old" ]; then
  echo "S1V pcache: run cache $new B < seed $old B; seed unchanged"; exit 0
fi
mkdir -p "$(dirname "$SEED")"
tmp="$SEED.tmp.$$"
cp "$NEW" "$tmp" && mv -f "$tmp" "$SEED" && echo "S1V pcache: seed $old -> $new B"
