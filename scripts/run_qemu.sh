#!/usr/bin/env bash
# Run the BMS on QEMU's MPS2-AN385 (Cortex-M3): bootloader + application,
# with the CAN UART bridged to Linux SocketCAN, so the Python simulator and
# harness talk to it exactly as they talk to the POSIX build on vcan0.
#
#   UART0 (log)  --------------------------------------> terminal, or --log FILE
#   UART1 (CAN)  -> pty -> slcand (N_SLCAN line discipline) -> SocketCAN slcan0
#
# Images: the bootloader is loaded with -kernel. The application slots and
# the metadata are NOT: QEMU copies every -kernel/-device loader image back
# into memory on each reset, which would undo an over-the-air update at the
# next reboot. Instead QEMU starts halted and scripts/qemu_flash.py programs
# them once through the GDB stub (like an SWD programmer), so they behave like
# flash: they survive resets and change only when the firmware writes them.
#
# Usage: scripts/run_qemu.sh [options]
#   --boot FILE     bootloader ELF                 (default build/qemu/bootloader.elf)
#   --slot-a FILE   image programmed into Slot A   (default build/qemu/app_slotA.signed.bin)
#   --slot-b FILE   image programmed into Slot B   (default: none, slot erased)
#   --iface NAME    SocketCAN interface name       (default slcan0)
#   --log FILE      write UART0 to FILE instead of following it in the terminal
#   --gdb-port N    GDB stub port, also used for programming (default 1234)
#   --icount OPTS   pass "-icount OPTS" to QEMU (see README, "Timing on QEMU")
#   --debug         stay halted at reset after programming; attach GDB to the port
#   --no-can        do not attach slcand (boot and log only)
#
# Prints "READY <iface>" once everything is up, then runs until QEMU exits or
# the script is interrupted; everything it started is cleaned up on exit.
# slcand and ip need root: the script uses sudo for those steps when not root.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
BOOT=build/qemu/bootloader.elf
SLOT_A=build/qemu/app_slotA.signed.bin
SLOT_B=""
IFACE=slcan0
LOG=""
GDB_PORT=1234
ICOUNT=""
DEBUG=0
CAN=1
QEMU="${QEMU:-qemu-system-arm}"
PYTHON="${PYTHON:-python3}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --boot)     BOOT="$2"; shift 2 ;;
        --slot-a)   SLOT_A="$2"; shift 2 ;;
        --slot-b)   SLOT_B="$2"; shift 2 ;;
        --iface)    IFACE="$2"; shift 2 ;;
        --log)      LOG="$2"; shift 2 ;;
        --gdb-port) GDB_PORT="$2"; shift 2 ;;
        --icount)   ICOUNT="$2"; shift 2 ;;
        --debug)    DEBUG=1; shift ;;
        --no-can)   CAN=0; shift ;;
        -h|--help)  sed -n '2,31p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

for f in "$BOOT" "$SLOT_A" ${SLOT_B:+"$SLOT_B"}; do
    [[ -f "$f" ]] || { echo "error: $f not found (run: make TARGET=qemu)" >&2; exit 1; }
done
command -v "$QEMU" >/dev/null || { echo "error: $QEMU not installed" >&2; exit 1; }
if (( CAN )); then
    command -v slcand >/dev/null || { echo "error: slcand not found (install can-utils)" >&2; exit 1; }
fi
SUDO=""
[[ $EUID -eq 0 ]] || SUDO=sudo

# Flash layout from the same header the firmware is built with.
mm() { awk -v n="$1" '$1 == "#define" && $2 == n { print $3 }' "$REPO/port/qemu-mps2/memory_map.h"; }
META_BASE=$(mm MM_META_BASE)
SLOT_A_BASE=$(mm MM_SLOT_A_BASE)
SLOT_B_BASE=$(mm MM_SLOT_B_BASE)
SLOT_SIZE=$(mm MM_SLOT_SIZE)
FLASH_END=$(( SLOT_B_BASE + SLOT_SIZE ))

WORK="$(mktemp -d)"
FOLLOW=0
if [[ -z "$LOG" ]]; then
    LOG="$WORK/uart0.log"
    FOLLOW=1
fi
: > "$LOG"

QEMU_PID=""
SLCAND_PID=""
cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$SLCAND_PID" ]]; then
        $SUDO kill "$SLCAND_PID" 2>/dev/null || true # slcand -c sends 'C' and detaches
    fi
    if [[ -n "$QEMU_PID" ]]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    # Wait for the interface to disappear so a quick restart can reuse the name.
    for _ in $(seq 50); do [[ -e "/sys/class/net/$IFACE" ]] || break; sleep 0.1; done
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

if (( CAN )) && [[ -e "/sys/class/net/$IFACE" ]]; then
    echo "error: interface $IFACE already exists (another run_qemu.sh still active?)" >&2
    exit 1
fi

# -S: start halted, so the slots can be programmed before the first instruction.
args=(-M mps2-an385 -cpu cortex-m3 -kernel "$BOOT" -S -gdb "tcp:127.0.0.1:$GDB_PORT"
      -display none -monitor none -serial "file:$LOG")
if (( CAN )); then
    args+=(-serial pty)   # QEMU allocates a pseudo-terminal and prints its name
else
    args+=(-serial null)
fi
[[ -n "$ICOUNT" ]] && args+=(-icount "$ICOUNT")

# QEMU's own messages (stdout and stderr: depending on the version the pty
# notice goes to either) are collected in qemu.out; UART0 goes to $LOG.
"$QEMU" "${args[@]}" > "$WORK/qemu.out" 2>&1 &
QEMU_PID=$!

# Factory programming: erase META and both slots (0xFF), program the images.
prog=(--port "$GDB_PORT" --erase "$META_BASE:$(( FLASH_END - META_BASE ))" --write "$SLOT_A_BASE:$SLOT_A")
[[ -n "$SLOT_B" ]] && prog+=(--write "$SLOT_B_BASE:$SLOT_B")
(( DEBUG )) || prog+=(--resume)
if ! "$PYTHON" "$HERE/qemu_flash.py" "${prog[@]}"; then
    cat "$WORK/qemu.out" >&2
    echo "error: programming the flash through the GDB stub failed" >&2
    exit 1
fi

if (( CAN )); then
    # "char device redirected to /dev/pts/N (label serial1)"
    PTY=""
    for _ in $(seq 150); do
        PTY="$(grep -o '/dev/pts/[0-9]*' "$WORK/qemu.out" | head -n1 || true)"
        [[ -n "$PTY" ]] && break
        kill -0 "$QEMU_PID" 2>/dev/null || { cat "$WORK/qemu.out" >&2; exit 1; }
        sleep 0.1
    done
    [[ -n "$PTY" ]] || { echo "error: QEMU did not report a pty" >&2; cat "$WORK/qemu.out" >&2; exit 1; }

    $SUDO modprobe slcan 2>/dev/null || true
    # -o: send 'O' (open), -c: send 'C' (close) on exit, -s6: 500 kbit/s
    # (meaningless on a UART link, but part of the handshake), -F: foreground.
    $SUDO slcand -o -c -s6 -F "$PTY" "$IFACE" &
    SLCAND_PID=$!
    for _ in $(seq 100); do [[ -e "/sys/class/net/$IFACE" ]] && break; sleep 0.1; done
    [[ -e "/sys/class/net/$IFACE" ]] || { echo "error: slcand did not create $IFACE" >&2; exit 1; }
    # Deeper transmit queue: the simulator sends 8 cell frames back to back,
    # and the default (10) can overflow while the UART drains.
    $SUDO ip link set "$IFACE" txqueuelen 1000 up
    # A real CAN bus arbitrates by ID: the update (0x6C0-0x6CF, the highest
    # IDs) always yields to measurements and commands. A UART is a FIFO, so
    # without help an update burst would queue ahead of the simulator's cell
    # data until the BMS (rightly) latched MEAS_TIMEOUT. A priority qdisc puts
    # the update IDs in the lowest band, restoring arbitration order on the
    # host side of the link.
    if $SUDO tc qdisc replace dev "$IFACE" root handle 1: prio bands 3 2>/dev/null &&
       $SUDO tc filter add dev "$IFACE" parent 1: prio 1 basic match 'canid(sff 0x6C0:0x7F0)' flowid 1:3 2>/dev/null; then
        echo "$IFACE: update IDs 0x6C0-0x6CF queued behind all other traffic (prio qdisc)" >&2
    else
        echo "warning: no prio qdisc / canid ematch on $IFACE: update traffic shares one FIFO with measurements" >&2
    fi
    echo "QEMU UART1 $PTY <-> $IFACE (slcand pid $SLCAND_PID)" >&2
fi

if (( DEBUG )); then
    echo "QEMU halted at reset with the flash programmed; GDB server on :$GDB_PORT. Attach with:" >&2
    echo "  gdb-multiarch $BOOT -ex 'add-symbol-file build/qemu/app_slotA.elf' -ex 'target remote :$GDB_PORT'" >&2
fi
echo "READY $IFACE log=$LOG gdb=$GDB_PORT"

if (( FOLLOW )); then
    tail -n +1 -f --pid="$QEMU_PID" "$LOG"
else
    wait "$QEMU_PID"
fi
