#!/bin/sh
# Fetches the part of ScummVM this door builds against.
#
# ScummVM is a big project and we only need one engine plus its dependency tail,
# so this does a sparse clone rather than vendoring 43 MB into the repository.
# The commit is pinned so a clean checkout always builds the same module.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/../third_party/scummvm"
COMMIT=3407d66ddf1e1135b56dab07667cd376d39ab2b3

# Only the directories the sky engine actually reaches. Measured, not guessed:
# engines/sky includes common (86 times), graphics (9), audio (8), engines (5),
# backends/keymapper (5), gui (4), image (2) and base (1).
PATHS="engines/sky common graphics audio image gui base backends/keymapper \
       backends/graphics backends/mixer backends/saves backends/timer \
       backends/events backends/mutex backends/fs math dists/engine-data"

# Applies every patch in tools/patches, skipping any already in the tree, so
# this is safe to re-run. These change GPL sources; the door's source offer
# covers them.
apply_patches() {
    for p in "$HERE"/patches/*.patch; do
        [ -e "$p" ] || continue
        if git apply --reverse --check "$p" 2>/dev/null; then
            echo "  patch already applied: $(basename "$p")"
        elif git apply "$p"; then
            echo "  applied $(basename "$p")"
        else
            echo "  !! FAILED to apply $(basename "$p")" >&2
            exit 1
        fi
    done
}

if [ -d "$DEST/.git" ]; then
    echo "ScummVM already fetched in $DEST"
    cd "$DEST"
    if [ "$(git rev-parse HEAD)" = "$COMMIT" ]; then
        echo "  at the pinned commit, nothing to do"
        apply_patches
        exit 0
    fi
    echo "  moving to the pinned commit $COMMIT"
else
    echo "Fetching ScummVM (sparse) into $DEST"
    mkdir -p "$DEST"
    cd "$DEST"
    git init -q
    git remote add origin https://github.com/scummvm/scummvm.git
    git config core.sparseCheckout true
fi

git sparse-checkout init --cone 2>/dev/null || true
# shellcheck disable=SC2086
git sparse-checkout set $PATHS
git fetch -q --depth 1 origin "$COMMIT"
git checkout -q "$COMMIT"

apply_patches

echo "ScummVM at $COMMIT"
echo "  sky engine:  $(find engines/sky -name '*.cpp' | wc -l) sources"
echo "  sky.cpt:     $(wc -c < dists/engine-data/sky.cpt) bytes (must be 419427)"
