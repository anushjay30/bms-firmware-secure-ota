#!/usr/bin/env bash
# Create and bring up a virtual CAN interface (default: vcan0).
# Usage: sudo scripts/setup_vcan.sh [ifname]
set -euo pipefail

IF="${1:-vcan0}"

if [[ $EUID -ne 0 ]]; then
    echo "error: needs root (try: sudo $0 $IF)" >&2
    exit 1
fi

if ! modprobe vcan 2>/dev/null; then
    cat >&2 <<'EOF'
error: could not load the 'vcan' kernel module.
  - Ubuntu (incl. GitHub runners): sudo apt-get install linux-modules-extra-$(uname -r)
  - WSL2: the stock Microsoft kernel has CONFIG_CAN_VCAN disabled; build a
    custom WSL kernel with CONFIG_CAN_VCAN=m (see README "WSL2 note").
EOF
    exit 1
fi

if ip link show "$IF" >/dev/null 2>&1; then
    echo "$IF already exists"
else
    ip link add dev "$IF" type vcan
fi
# MTU 16 = classic CAN frames only (72 would enable CAN FD).
ip link set "$IF" mtu 16
ip link set up "$IF"
ip -details link show "$IF"
