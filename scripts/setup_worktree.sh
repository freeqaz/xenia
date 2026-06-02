#!/bin/bash
#
# setup_worktree.sh — create a buildable git worktree of the Xenia tree, cheaply
# (copy-on-write reflinks).
#
# Why this exists
# ---------------
# Xenia's entire build tree lives under `build/` (premake-generated GNU
# makefiles + a ~4-9 GB object cache in build/obj/Linux/<config>). `build/` is
# gitignored (.gitignore: `/build/`), so a naive `git worktree add` gives you a
# worktree with NO build dir at all: no makefiles, no object cache, no binary.
# A from-scratch `xb premake && make` in that worktree would take many minutes
# and ~9 GB of fresh objects.
#
# This repo lives on btrfs, which supports `cp --reflink=auto` (copy-on-write).
# Reflink-copying `build/` is effectively instant and consumes ~no extra disk
# until files diverge, so each worktree gets a PRIVATE, WARM build dir and
# incremental builds only recompile what actually changed.
#
# Why a full reflink copy of build/ "just works"
# ----------------------------------------------
# The premake makefiles are invoked from inside `build/` (`make -C build ...`)
# and reference sources with paths RELATIVE to build/:
#     ../src   ../third_party   -I..  -I../src
# and emit objects/binaries to RELATIVE dirs:
#     obj/Linux/<config>/...    bin/Linux/<config>/xenia-headless
# The .d dependency files are likewise fully relative (`../src/...`). The only
# absolute include paths are read-only system headers (`-I/usr/include/...`).
# Therefore, once `build/` sits at `<worktree>/build/`, `../src` resolves to the
# worktree's OWN src tree and every output lands inside the worktree. Nothing
# points back into the main repo, so this worktree's build can never corrupt the
# main build dir.
#
# Usage:
#   scripts/setup_worktree.sh <worktree-path> <branch-or-commit> [--checked-only]
#
# Arguments:
#   worktree-path   Where to create the worktree (required).
#   branch-or-commit
#                   Ref to base the worktree on. If it names an existing branch,
#                   that branch is checked out; otherwise a new branch
#                   `wt-<basename>` is created at that ref (required).
#   --checked-only  Only reflink the Checked config's object cache (drops the
#                   Debug/Release obj trees). Saves disk on non-CoW fallbacks;
#                   on btrfs the full copy is already free, so this is optional.
#
# Examples:
#   scripts/setup_worktree.sh /tmp/xenia-nui-probe HEAD
#   scripts/setup_worktree.sh ../xenia-wt-fix headless-vulkan-linux
#
# Prerequisite: the main repo must have been built at least once (so build/ has
# generated makefiles + an object cache to reflink).

set -euo pipefail

MAIN_REPO="$(cd "$(dirname "$0")/.." && pwd)"

# ---- args -------------------------------------------------------------------
POSITIONAL=()
CHECKED_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --checked-only) CHECKED_ONLY=1 ;;
        -h|--help)
            sed -n '2,46p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) POSITIONAL+=("$arg") ;;
    esac
done

if [ "${#POSITIONAL[@]}" -lt 2 ]; then
    echo "ERROR: usage: scripts/setup_worktree.sh <worktree-path> <branch-or-commit> [--checked-only]" >&2
    exit 1
fi

WORKTREE_PATH="${POSITIONAL[0]}"
BASE_REF="${POSITIONAL[1]}"

# Resolve the base ref to a concrete commit for clarity.
BASE_COMMIT="$(git -C "$MAIN_REPO" rev-parse --short "$BASE_REF" 2>/dev/null)" || {
    echo "ERROR: cannot resolve ref '$BASE_REF'" >&2
    exit 1
}

# ---- sanity: the main repo must have a populated build/ ---------------------
MAIN_BUILD="$MAIN_REPO/build"
if [ ! -d "$MAIN_BUILD" ] || [ ! -f "$MAIN_BUILD/Makefile" ]; then
    echo "ERROR: $MAIN_BUILD/Makefile missing — run 'xb premake && make -C build' in the main repo first." >&2
    exit 1
fi

# Warn if the destination isn't on a reflink-capable fs (script still works,
# just slow + space-hungry because cp falls back to full copies).
DEST_FSTYPE="$(findmnt -no FSTYPE --target "$(dirname "$WORKTREE_PATH")" 2>/dev/null || echo unknown)"
case "$DEST_FSTYPE" in
    btrfs|xfs|zfs) : ;;
    *) echo "WARN: $(dirname "$WORKTREE_PATH") is on '$DEST_FSTYPE'; reflinks may be unavailable — the build/ copy will be a full ~9 GB copy (slow)." >&2 ;;
esac

# ---- worktree (idempotent) --------------------------------------------------
if [ -e "$WORKTREE_PATH/.git" ]; then
    echo "==> Worktree already exists at $WORKTREE_PATH (reconfiguring build/ in place)"
else
    echo "==> Creating worktree at $WORKTREE_PATH"
    echo "    base=$BASE_REF ($BASE_COMMIT)"
    if git -C "$MAIN_REPO" show-ref --verify --quiet "refs/heads/$BASE_REF"; then
        # Existing branch: check it out directly.
        git -C "$MAIN_REPO" worktree add "$WORKTREE_PATH" "$BASE_REF"
    else
        # Commit/tag/remote-ref: create a fresh local branch for the worktree.
        BRANCH="wt-$(basename "$WORKTREE_PATH")"
        git -C "$MAIN_REPO" worktree add "$WORKTREE_PATH" -b "$BRANCH" "$BASE_REF"
    fi
fi

# ---- build/ : reflink copy (CoW). Private + warm object cache ---------------
WT_BUILD="$WORKTREE_PATH/build"
echo "==> build/  (reflink copy — private, warm object cache)"
rm -rf "$WT_BUILD"
mkdir -p "$WORKTREE_PATH"
cp -a --reflink=auto "$MAIN_BUILD" "$WT_BUILD"

# Optionally drop the configs we won't build to reclaim space on non-CoW fs.
if [ "$CHECKED_ONLY" -eq 1 ]; then
    echo "==> --checked-only: dropping Debug/Release object trees"
    rm -rf "$WT_BUILD/obj/Linux/Debug" "$WT_BUILD/obj/Linux/Release" \
           "$WT_BUILD/bin/Linux/Debug" "$WT_BUILD/bin/Linux/Release" 2>/dev/null || true
fi

# ---- third_party submodules : reflink copy (read-only pinned sources) -------
# `git worktree add` does NOT populate submodules — they come up empty. The
# build needs their .cc sources (e.g. third_party/fmt/src/format.cc). Reflink
# the main repo's checked-out submodule working trees in; this gives the EXACT
# sources the reflinked object cache was built against (including any locally
# patched submodule content), for free on CoW.
echo "==> third_party submodules  (reflink copy — pinned sources)"
while read -r _ SUBPATH; do
    [ -n "$SUBPATH" ] || continue
    SRC="$MAIN_REPO/$SUBPATH"
    # Only copy submodules that are actually populated in the main repo.
    [ -d "$SRC" ] && [ -n "$(ls -A "$SRC" 2>/dev/null)" ] || continue
    DST="$WORKTREE_PATH/$SUBPATH"
    rm -rf "$DST"
    mkdir -p "$(dirname "$DST")"
    cp -a --reflink=auto "$SRC" "$DST"
done < <(git -C "$MAIN_REPO" config --file .gitmodules --get-regexp path | awk '{print $1, $2}')

# ---- warm the object cache : stamp it newer than the source tree ------------
# The build uses make (mtime-based). `git worktree add` writes every source
# file with a FRESH mtime, so without this every object looks stale and the
# whole tree (~600 TUs) rebuilds — defeating the warm reflinked cache. Stamp
# all cached objects/archives/binaries forward so make treats unchanged TUs as
# up to date. Any source you subsequently edit gets an even newer mtime and
# rebuilds correctly (validated: touch one .cc -> exactly one recompile).
#
# Caveat: if the main build dir was built against UNCOMMITTED working-tree
# changes, the objects for those specific files encode that WIP, while the
# worktree's sources are at the committed ref. Stamping forward would let make
# skip rebuilding them. This is harmless for the intended use (you edit those
# files in the worktree anyway, which re-triggers their compile); if you need a
# guaranteed-clean object for a WIP file, `touch` its .cc before building.
#
# Stronger caveat: if the committed ref does NOT build standalone because the
# WIP introduced cross-file dependencies (e.g. emulator.cc calls a function
# only declared in an uncommitted header), the worktree won't compile from the
# committed sources alone. In that case overlay the main repo's WIP files into
# the worktree first, e.g.:
#   cd <main-repo>
#   for f in $(git status --porcelain | awk '/^ M/{print $2}' \
#               | grep -E '\.(cc|h|inc)$'); do
#     cp --reflink=auto "$f" "<worktree>/$f"; done
# This is a read-only copy out of the main tree (it never writes to main).
echo "==> Warming object cache (stamp objects newer than sources)"
find "$WT_BUILD/obj" "$WT_BUILD/bin" -type f \
    \( -name '*.o' -o -name '*.a' -o -name '*.d' \
       -o -name 'xenia-headless' -o -name 'xenia' \) \
    -exec touch {} + 2>/dev/null || true

# ---- safety assertion : worktree build/ must be its own real dir ------------
if [ -L "$WT_BUILD" ]; then
    echo "FATAL: $WT_BUILD is a symlink — the build would corrupt the main tree. Aborting." >&2
    exit 1
fi

echo ""
echo "Worktree ready:  $WORKTREE_PATH"
echo "  base:          $BASE_REF ($BASE_COMMIT)"
echo ""
echo "Build (incremental, warm cache):"
echo "  make -C $WT_BUILD xenia-headless config=checked_linux -j\$(nproc)"
echo ""
echo "Binary lands at:"
echo "  $WT_BUILD/bin/Linux/Checked/xenia-headless"
echo ""
echo "Remove when done:  git -C $MAIN_REPO worktree remove --force $WORKTREE_PATH"
