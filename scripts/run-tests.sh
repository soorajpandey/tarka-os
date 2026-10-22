#!/usr/bin/env bash
# Boot the self-test ISO in QEMU and turn the result into an exit status.
set -euo pipefail

ISO="${1:?usage: run-tests.sh <iso>}"
TIMEOUT="${TEST_TIMEOUT:-60}"
LOG="$(dirname "$ISO")/selftest.log"

set +e
timeout "$TIMEOUT" qemu-system-x86_64 \
    -M q35 -cpu max -m 256M \
    -cdrom "$ISO" \
    -display none -serial stdio -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    | tee "$LOG"
status=${PIPESTATUS[0]}
set -e

case "$status" in
    33) echo "==> self-tests passed"; exit 0 ;;
    35) echo "==> self-tests FAILED" >&2; exit 1 ;;
    124) echo "==> self-tests timed out after ${TIMEOUT}s" >&2; exit 1 ;;
    *) echo "==> QEMU exited unexpectedly with status $status" >&2; exit 1 ;;
esac
