#!/bin/bash
# build_content.sh -- (re)build the pinned, NON-GIT content directory the
# fork-regress scenarios run against.
#
#   tools/fork-regress/build_content.sh [content-dir]
#
# Default content dir: $FORK_REGRESS_CONTENT, else /home/free/tmp/fork-regress-content
# (deliberately NOT under /tmp: a reboot wipes /tmp, and the old /tmp/rb3tu5boot
# name meant three different things over time).
#
# NOTHING this script produces may enter git. It copies retail executables and
# derives a 64-byte decryption table; the repo is public.
#
# Idempotent: every pinned input is verified against an expected sha256 before
# it is copied, and an existing copy is re-verified rather than overwritten. A
# mismatch is fatal -- a content dir name must never mean two different things.
#
# Sources (all read-only):
#   DC3 original    dc3-decomp/orig/373307D9/debug.xex
#   DC3 ymca flow   dc3-decomp/scripts/dc3-input-flows/xenia-ymca.txt
#   DC3 symbols     dc3-decomp git: config/373307D9/symbols.txt @ c362ede1c (oracle era)
#                   and @ 3504a8a58 (627-trap era, the copy in force on 2026-08-29)
#   DC3 decomp xex  dc3-decomp/build/373307D9/default.xex + xenia_dc3_patch_manifest.json
#                   (written 2026-08-24 10:19 and unchanged since; any build_xex.py
#                   run overwrites them, hence the copy)
#   fingerprints    xenia git: docs/dc3-boot/dc3_nui_fingerprints.txt @ a5fc2f1b6
#   RB3 TU5 clean   rb3-xenon/_tu5probe/clean/clean_tu5_nodd.xex
#   RB3DX           /srv/torrents/games/arbys/rb3/default.xex
#   RB3 disc data   /srv/torrents/games/arbys/rb3/{gen/*,AvatarAwards,nxeart,charnames.zbm}
#   mogg key table  64 bytes at VA 0x82C76258 of rb3-xenon/orig/45410914/band.exe
#                   (the RB3DX-lineage, already-deobscured image; see
#                   docs/jit-fault-wiki/WORKSTREAM-rb3-on-xenia-bringup.md §8x)
#   DC1 (inertness) milo-executable-library/dc1/TU0/default.xex
set -euo pipefail

C="${1:-${FORK_REGRESS_CONTENT:-/home/free/tmp/fork-regress-content}}"
DC3="${DC3_DECOMP:-/home/free/code/milohax/dc3-decomp}"
XENIA_GIT="${XENIA_GIT:-/home/free/code/milohax/xenia}"
RB3X="${RB3_XENON:-/home/free/code/milohax/rb3-xenon}"
RB3SRC="${RB3_DISC:-/srv/torrents/games/arbys/rb3}"
MEL="${MILO_EXE_LIB:-/home/free/code/milohax/milo-executable-library}"

case "$C" in
  /tmp/*) echo "refusing a content dir under /tmp (reboot wipes it): $C" >&2; exit 2 ;;
esac
if git -C "$C" rev-parse --show-toplevel >/dev/null 2>&1 || \
   git -C "$(dirname "$C")" rev-parse --show-toplevel >/dev/null 2>&1; then
  echo "refusing a content dir inside a git work tree: $C" >&2; exit 2
fi

die() { echo "build_content: $*" >&2; exit 1; }
sha() { sha256sum "$1" | awk '{print $1}'; }

# pin <src> <dst> <expected-sha256-or-empty>
pin() {
  local src="$1" dst="$2" want="$3" got
  [ -f "$src" ] || die "missing source $src"
  got="$(sha "$src")"
  if [ -n "$want" ] && [ "$got" != "$want" ]; then
    die "$src sha256 $got != expected $want"
  fi
  mkdir -p "$(dirname "$dst")"
  if [ -f "$dst" ]; then
    [ "$(sha "$dst")" = "$got" ] || die "$dst exists with different content (refusing to overwrite)"
  else
    cp --reflink=auto "$src" "$dst"
    chmod a-w "$dst"
  fi
  echo "  pinned $(basename "$dst")  ${got:0:16}"
}

# pin_git <repo> <rev> <path> <dst>
pin_git() {
  local repo="$1" rev="$2" path="$3" dst="$4" tmp
  tmp="$(mktemp)"
  git -C "$repo" show "$rev:$path" > "$tmp" || die "git show $rev:$path in $repo"
  pin "$tmp" "$dst" ""
  rm -f "$tmp"
}

# link_disc <dir> <include-patch-ark: 0|1>
# gen/ is a REAL directory of per-file symlinks: the torrent's own gen/ holds a
# self-loop symlink gen/gen -> gen that a recursive walk never leaves.
link_disc() {
  local d="$1" patch="$2" f b
  mkdir -p "$d/gen"
  for f in "$RB3SRC"/gen/*; do
    b="$(basename "$f")"
    [ -L "$f" ] && continue                      # skip gen/gen self-loop
    if [ "$patch" = 0 ]; then
      case "$b" in patch_xbox*) continue ;; esac
    fi
    ln -sfn "$f" "$d/gen/$b"
  done
  for b in AvatarAwards nxeart charnames.zbm; do
    ln -sfn "$RB3SRC/$b" "$d/$b"
  done
}

echo "content dir: $C"
mkdir -p "$C"

echo "[dc3] original layout (S1/S1V/S2): debug.xex + the game data next to it"
# The xex's directory is mounted as game:\ -- the title reads its arks,
# grammars and NUI speech packs from there. dc3-decomp/orig/373307D9 holds them
# as symlinks into orig-assets; mirror exactly those (a dir with the xex alone
# boot-hangs: guest thread 6 is created and never runs).
O="$C/dc3-original"
pin "$DC3/orig/373307D9/debug.xex" "$O/debug.xex" \
    2d5e4a320aabf272ef21f1ac6ae6518460fdf4892ec88fd8920f770bf29c4728
for f in "$DC3"/orig/373307D9/*; do
  b="$(basename "$f")"
  [ -L "$f" ] || continue
  case "$b" in *.xex|*.exe) continue ;; esac
  ln -sfn "$(readlink -f "$f")" "$O/$b"
done
echo "  linked $(find "$O" -maxdepth 1 -type l | wc -l) data entries"
I="$C/dc3-inputs"
pin "$DC3/scripts/dc3-input-flows/xenia-ymca.txt" "$I/xenia-ymca.txt" \
    ea733eeeb809dc91fe4d97050866579364063a9b37e0e04f3f50519534331879
pin_git "$DC3" c362ede1c config/373307D9/symbols.txt "$I/symbols.dc3-decomp-c362ede1c.txt"
pin_git "$XENIA_GIT" a5fc2f1b6 docs/dc3-boot/dc3_nui_fingerprints.txt \
    "$I/dc3_nui_fingerprints.xenia-a5fc2f1b6.txt"

echo "[dc3] decomp layout 2026-08-24 (S3, 627 forced traps)"
D="$C/dc3-decomp-2026-08-24"
pin "$DC3/build/373307D9/default.xex" "$D/default.xex" \
    b4af75f8bb67b7d31ac93c5a0144465229d2c8258bcb7a864ba1c834ac389ba1
pin "$DC3/build/373307D9/xenia_dc3_patch_manifest.json" "$D/xenia_dc3_patch_manifest.json" \
    08237293a2561567fe4e40518748d033544feb40af1e87fa228aad07cfe53029
pin_git "$DC3" 3504a8a58 config/373307D9/symbols.txt "$D/symbols.dc3-decomp-3504a8a58.txt"
pin_git "$XENIA_GIT" a5fc2f1b6 docs/dc3-boot/dc3_nui_fingerprints.txt \
    "$D/dc3_nui_fingerprints.xenia-a5fc2f1b6.txt"
# dc3-decomp/build/373307D9 (where every 627 run booted from) carries gen ->
# orig-assets/gen next to default.xex; mirror it.
ln -sfn "$(readlink -f "$DC3/build/373307D9/gen")" "$D/gen"

echo "[rb3] clean TU5, dirty-disc bypass (S4) -- pristine disc data, NO patch ark"
pin "$RB3X/_tu5probe/clean/clean_tu5_nodd.xex" "$C/rb3/tu5-clean-nodd/default.xex" \
    6d73992c4b7dd81fddd62e733312c5780153324eb4a7baf8d7cc44aba4fc2331
link_disc "$C/rb3/tu5-clean-nodd" 0

echo "[rb3] RB3DX (S5) -- disc data incl. the RB3DX patch ark"
pin "$RB3SRC/default.xex" "$C/rb3/rb3dx/default.xex" \
    6639ce25745505b598480499ca53b421fdec5604d813f5ee2c8152ecdad2a5ea
link_disc "$C/rb3/rb3dx" 1

echo "[rb3] mogg key table (64 B @ VA 0x82C76258 of the deobscured DX-lineage image)"
KEY="$C/rb3/mogg_key_table.hex"
/usr/bin/python3 - "$RB3X/orig/45410914/band.exe" "$KEY.new" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', data, 0x3C)[0]
assert data[pe:pe+4] == b'PE\0\0', 'not a PE'
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsz = struct.unpack_from('<H', data, pe + 20)[0]
opt = pe + 24
base = struct.unpack_from('<I', data, opt + 28)[0]
va = 0x82C76258
for i in range(nsec):
    s = opt + optsz + i * 40
    name = data[s:s+8].rstrip(b'\0').decode()
    vsz, sva, rsz, rptr = struct.unpack_from('<IIII', data, s + 8)
    if base + sva <= va < base + sva + max(vsz, rsz):
        off = rptr + (va - base - sva)
        key = data[off:off+64]
        assert len(key) == 64
        open(sys.argv[2], 'w').write(key.hex() + '\n')
        print(f'  {name} va=0x{va:08X} file_off=0x{off:X}')
        break
else:
    raise SystemExit('VA not in any section')
PY
# Retail TU5 ships an OBSCURED table at the same VA; only the DX image's
# deobscured table decrypts (dx_vs_retail_diff.txt). Verify by hash -- no
# byte of the table is written into this public repo -- and refuse anything
# else rather than run a scenario that silently cannot decrypt.
if [ "$(sha "$KEY.new")" != 4321690f541eb89b71dbd94f43385919a439acb6afbc4674dd1881a68b302fb4 ]; then
  rm -f "$KEY.new"; die "band.exe key table is not the expected DX-lineage table"
fi
if [ -f "$KEY" ]; then
  cmp -s "$KEY" "$KEY.new" || { rm -f "$KEY.new"; die "$KEY exists with different content"; }
  rm -f "$KEY.new"
else
  mv "$KEY.new" "$KEY"; chmod 400 "$KEY"
fi
echo "  key table sha256 $(sha "$KEY" | cut -c1-16)"

echo "[rb3] content seed: the RB3 profile content as the 2026-08-29 s66 run left it"
# s66 ran against the SHARED storage root. Its content dir held globaloptions,
# songcache, rbdxcache and band3/save.dat (save.dat rewritten by s66's own boot
# autosave at 01:27). Measured 2026-10-01: a FRESH content root boots to
# splash_screen but the A-press join never fires (fresh-profile path), and
# globaloptions WITHOUT band3 parks the boot in the startup autosave (the
# CREATE-DATA dialog case). So the seed is all four files. Read, never written.
SEED_SRC="${RB3_SEED_SRC:-$HOME/.local/share/Xenia/content/45410914/00000001}"
SEED="$C/rb3/seed-post-s66-2026-08-29/content/45410914/00000001"
pin "$SEED_SRC/globaloptions/globaloptions" "$SEED/globaloptions/globaloptions" \
    117ab272b069c124149bbdb4edaebbda35910a573fcc7baccbf625942c62222d
pin "$SEED_SRC/songcache/songcache" "$SEED/songcache/songcache" \
    9b992384f7194cb721e3deb70813ca531757c558377aa83502b31b8e535ec803
pin "$SEED_SRC/rbdxcache/rbdxcache" "$SEED/rbdxcache/rbdxcache" \
    9b992384f7194cb721e3deb70813ca531757c558377aa83502b31b8e535ec803
pin "$SEED_SRC/band3/save.dat" "$SEED/band3/save.dat" \
    3fc8bd7820fb696dd5bcac2936365b2dc998e4b124485a1d92b7efb9b38c1d52

echo "[dc1] inertness target (S6)"
pin "$MEL/dc1/TU0/default.xex" "$C/dc1/default.xex" \
    a8f893542013464b7bd29fc0b2fb654fe80b1e356cd68c3c9ffd823e1e07892b

echo "[manifest]"
(
  cd "$C"
  # Real files: sha256. Symlinked disc data (~6 GB): size + mtime + target,
  # which is what changes if the torrent dir is re-downloaded.
  find . -type f ! -name MANIFEST.sha256 ! -path './s1v-pcache/*' -printf '%P\n' | sort | while read -r f; do
    printf '%s  %s\n' "$(sha "$f")" "$f"
  done
  find . -type l -printf '%P\n' | sort | while read -r f; do
    printf 'symlink %s -> %s size=%s mtime=%s\n' "$f" "$(readlink "$f")" \
      "$(stat -L -c %s "$f")" "$(stat -L -c %Y "$f")"
  done
) > "$C/MANIFEST.sha256"
echo "  $(grep -vc '^symlink' "$C/MANIFEST.sha256") files, $(grep -c '^symlink' "$C/MANIFEST.sha256") symlinks -> $C/MANIFEST.sha256"

# S1V's warm pipeline cache is NOT made here: it is a run output (the first S1V
# PASS writes it; README "S1V pipeline cache") and it is mutable, so it stays
# out of the manifest above.
if [ -s "$C/s1v-pcache/xenia_vulkan_pipeline_cache.bin" ]; then
  echo "[s1v] warm pipeline cache: $(stat -c %s "$C/s1v-pcache/xenia_vulkan_pipeline_cache.bin") B (not in the manifest)"
else
  echo "[s1v] no warm pipeline cache yet: the first S1V PASS writes $C/s1v-pcache/ (README \"S1V pipeline cache\")"
fi
