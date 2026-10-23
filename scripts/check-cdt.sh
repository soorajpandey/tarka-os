#!/usr/bin/env bash
# ADR 0010 implementation gates, checked on the kernel sources (the
# self-test file is test-only and is exempt).
#
# SC31.2: "last capability to an object" is decided only by the live-record
#   count (SC19a). The stage 1 positional helpers must not come back, and
#   no code may compare the objects of CDT neighbours.
# SC31.5: every capability record enters the CDT through one of the two
#   commit functions that enforce the derivation freeze (SC22b F1):
#   cap_derive_commit() in cap.c and cap_retype() in untyped.c.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
srcs=$(find kernel -name '*.c' -o -name '*.h' -o -name '*.S' | grep -v '^kernel/core/selftest.c$' | sort)

# SC31.2 (a): the positional helpers.
hits=$(grep -nwE 'is_final|cap_object_first|cap_object_next|cap_same_object' $srcs || true)
if [ -n "$hits" ]; then
    echo "SC31.2: positional finality helper in kernel sources:" >&2
    echo "$hits" >&2
    fail=1
fi

# SC31.2 (b): a same-object comparison involving a CDT neighbour.
hits=$(grep -nE '(\.(open|close)\.(prev|next)|cdt_elem_cap|cap_first_child).*object|object.*(\.(open|close)\.(prev|next)|cdt_elem_cap|cap_first_child)' $srcs || true)
if [ -n "$hits" ]; then
    echo "SC31.2: object comparison with a CDT neighbour:" >&2
    echo "$hits" >&2
    fail=1
fi

# SC31.2 (c): the capability core never compares objects at all.
hits=$(grep -nE 'object[[:space:]]*(==|!=)|(==|!=)[[:space:]]*[a-z_>.-]*object\b' kernel/core/cap.c || true)
if [ -n "$hits" ]; then
    echo "SC31.2: object comparison in the capability core:" >&2
    echo "$hits" >&2
    fail=1
fi

# SC31.5: callers of cap_insert_child(), by enclosing function. A function
# definition starts in column 0 with an identifier and '('; the call sites
# are every other line naming cap_insert_child(.
calls=$(awk '
    /^[A-Za-z_][A-Za-z0-9_ \t\*]*\(/ && !/;[[:space:]]*$/ {
        fn = $0; sub(/\(.*/, "", fn); n = split(fn, w, /[ \t\*]+/); fn = w[n]
        if (fn == "cap_insert_child") next
    }
    /cap_insert_child\(/ && fn != "cap_insert_child" && !/^[[:space:]]*(\/\*|\*)/ {
        print FILENAME ":" fn
    }' $srcs | grep -v '\.h:' | sort)
want=$'kernel/core/cap.c:cap_derive_commit\nkernel/core/untyped.c:cap_retype'
if [ "$calls" != "$want" ]; then
    echo "SC31.5: cap_insert_child() must be called exactly from cap_derive_commit and cap_retype; found:" >&2
    echo "$calls" >&2
    fail=1
fi

[ $fail -eq 0 ] || exit 1
echo "cdt: finality by count only; records inserted only by derivation and retype commits"
