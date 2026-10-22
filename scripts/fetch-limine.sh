#!/usr/bin/env bash
# Fetch the Limine bootloader binaries at a pinned commit.
#
# The commit hash is the trust anchor: git verifies every object against it,
# so a compromised mirror or branch force-push cannot substitute different
# bootloader binaries. Bump LIMINE_COMMIT deliberately, in its own commit,
# and update kernel/third_party/limine/limine.h from the same revision.
set -euo pipefail

LIMINE_REPO="https://github.com/limine-bootloader/limine.git"
LIMINE_COMMIT="ee5d29cd0a8034612dcd1df3f00052480db785c5" # v9.x-binary
DEST="${1:-third_party/limine}"

if [ -d "$DEST/.git" ] && [ "$(git -C "$DEST" rev-parse HEAD)" = "$LIMINE_COMMIT" ]; then
    exit 0
fi

rm -rf "$DEST"
mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" fetch -q --depth 1 "$LIMINE_REPO" "$LIMINE_COMMIT"
git -C "$DEST" checkout -q FETCH_HEAD

actual="$(git -C "$DEST" rev-parse HEAD)"
if [ "$actual" != "$LIMINE_COMMIT" ]; then
    echo "limine: expected $LIMINE_COMMIT, got $actual" >&2
    exit 1
fi
echo "limine: pinned at $LIMINE_COMMIT"
