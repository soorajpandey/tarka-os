#!/usr/bin/env bash
# ADR 0010 G2 (revision 3f): kernel/core/sched.c is the only writer of a
# thread's scheduling state: t->state, the run-queue links, home_cpu, and
# the per-CPU `current`. Everyone else asks for a transition through a
# sched_* primitive, which checks the edge and commits the state together
# with run-queue membership (G11). The self-test file is test-only and is
# exempt: it corrupts these fields on purpose to prove the audit notices.
#
# A write is an assignment (plain or compound), an increment or decrement,
# or taking the field's address (which would let a write happen
# elsewhere). The address-of form is recognised where a unary & can
# stand: after ( , = ; { or return, or at the start of a line.
set -euo pipefail
cd "$(dirname "$0")/.."

fields='state|current|rq_next|rq_prev|home_cpu'
srcs=$(find kernel -name '*.c' -o -name '*.h' -o -name '*.S' |
       grep -vE '^kernel/core/(sched|selftest)\.c$' | sort)

hits=$(grep -nE "(->|\.)($fields)[[:space:]]*([-+*/%|&^]|<<|>>)?=([^=]|$)|(^|[(,=;{]|return)[[:space:]]*&[[:space:]]*[A-Za-z_][A-Za-z0-9_]*(\([^)]*\))?((->|\.)[A-Za-z_][A-Za-z0-9_]*)*(->|\.)($fields)\b|(\+\+|--)[[:space:]]*[A-Za-z_][A-Za-z0-9_>().-]*(->|\.)($fields)\b|(->|\.)($fields)[[:space:]]*(\+\+|--)" $srcs || true)
if [ -n "$hits" ]; then
    echo "G2: scheduler state written outside kernel/core/sched.c:" >&2
    echo "$hits" >&2
    exit 1
fi
# Revision 3g H1, A2: the quantum timer is scheduler state. Programming
# the LAPIC timer (setup, arm, stop) is allowed only in sched.c, and the
# self-tests are not exempt: they go through the scheduler. Reading the
# count is allowed in sched.c and, to test the hardware's semantics, in
# the self-tests. lapic.c and lapic.h define them.
all=$(find kernel -name '*.c' -o -name '*.h' -o -name '*.S' |
      grep -vE '^kernel/arch/x86_64/lapic\.[ch]$' | sort)
prog=$(grep -nwE 'lapic_timer_(setup|arm|stop)' $all | grep -v '^kernel/core/sched\.c:' || true)
readc=$(grep -nwE 'lapic_timer_(count|divide)' $all | grep -vE '^kernel/core/(sched|selftest)\.c:' || true)
if [ -n "$prog$readc" ]; then
    echo "H1: the LAPIC timer used outside kernel/core/sched.c:" >&2
    [ -n "$prog" ] && echo "$prog" >&2
    [ -n "$readc" ] && echo "$readc" >&2
    exit 1
fi
echo "sched: thread state, run queue, home CPU, current and the timer owned by sched.c"
