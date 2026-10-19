#!/usr/bin/env bash
# The whole story in one command (Linux, after `make TARGET=qemu`):
#
#   boot v1 -> simulator running -> overtemperature -> contactors open ->
#   reset -> OTA update to v2 -> v2 confirms itself -> bad update rejected
#
# Starts QEMU (bootloader + slot A) with slcand on slcan0 via run_qemu.sh,
# then narrates the steps with scripts/demo_story.py, showing the firmware's
# own log lines as it goes. Record it with:
#   asciinema rec demo.cast -c "bash scripts/demo.sh"
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
IFACE="${IFACE:-slcan0}"
PYTHON="${PYTHON:-python3}"
cd "$REPO"

for f in build/qemu/bootloader.elf build/qemu/app_slotA.signed.bin build/qemu/app_slotB.bin keys/dev_ed25519.pem; do
    [[ -f "$f" ]] || { echo "error: $f missing - run: make TARGET=qemu" >&2; exit 1; }
done

LOG="$(mktemp)"
OUT="$(mktemp)"
bash "$HERE/run_qemu.sh" --iface "$IFACE" --log "$LOG" > "$OUT" 2>&1 &
RUN_PID=$!
cleanup() {
    kill "$RUN_PID" 2>/dev/null || true
    wait "$RUN_PID" 2>/dev/null || true
    rm -f "$LOG" "$OUT"
}
trap cleanup EXIT

echo "Starting QEMU (Cortex-M3 mps2-an385): bootloader + BMS v1.0.0 in slot A, CAN on $IFACE ..."
for _ in $(seq 300); do
    grep -q "^READY" "$OUT" && grep -q "BMS ready" "$LOG" && break
    kill -0 "$RUN_PID" 2>/dev/null || { cat "$OUT"; exit 1; }
    sleep 0.1
done
grep -q "BMS ready" "$LOG" || { cat "$OUT" "$LOG"; echo "QEMU did not come up" >&2; exit 1; }
grep -E "^BOOT|MAIN   (BMS firmware|image)" "$LOG" | sed 's/^/   | /'

"$PYTHON" "$HERE/demo_story.py" --log "$LOG" --channel "$IFACE"
