#!/usr/bin/env bash
# Headless boot check for CI: bootloader + Slot A in QEMU (mps2-an385) with no
# CAN link; wait for "BMS ready" on the log UART (UART0).
#
# Usage: scripts/qemu_smoke.sh [timeout-seconds] [-- extra run_qemu.sh options]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIMEOUT="${1:-30}"
shift || true
[[ "${1:-}" == "--" ]] && shift

LOG="$(mktemp)"
bash "$HERE/run_qemu.sh" --no-can --log "$LOG" --gdb-port "${BMS_QEMU_GDB_PORT:-1234}" "$@" > /dev/null &
PID=$!
trap 'kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; rm -f "$LOG"' EXIT

deadline=$((SECONDS + TIMEOUT))
while (( SECONDS < deadline )); do
    if grep -qE "ASSERT FAILED|HARD FAULT|STACK OVERFLOW|MALLOC FAILED|BOOT  FATAL|NO VALID IMAGE" "$LOG"; then
        cat "$LOG"
        echo "SMOKE FAILED: fatal error during boot" >&2
        exit 1
    fi
    if grep -q "BMS ready" "$LOG"; then
        sleep 1.5 # also capture the startup stack report (printed ~1 s after boot)
        cat "$LOG"
        echo "SMOKE OK: 'BMS ready' seen"
        exit 0
    fi
    kill -0 "$PID" 2>/dev/null || { cat "$LOG"; echo "SMOKE FAILED: QEMU exited" >&2; exit 1; }
    sleep 0.2
done

cat "$LOG"
echo "SMOKE FAILED: no 'BMS ready' within ${TIMEOUT}s" >&2
exit 1
