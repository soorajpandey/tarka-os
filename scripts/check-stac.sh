#!/usr/bin/env bash
# ADR 0008, T26: SMAP is opened (stac) only by the user-copy routine.
# The self-test file is test-only and probes mappings directly.
set -euo pipefail
hits=$(grep -rnE '(^[[:space:]]*stac\b|"[[:space:]]*stac\b)' kernel --include=*.c --include=*.S --include=*.h \
        | grep -v '^kernel/arch/x86_64/copy_user.S:' \
        | grep -v '^kernel/core/selftest.c:' || true)
if [ -n "$hits" ]; then
    echo "stac outside the user-copy routine:" >&2
    echo "$hits" >&2
    exit 1
fi
echo "stac: only in copy_user.S (and test-only code)"
