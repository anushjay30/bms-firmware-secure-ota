"""Measure how regular the firmware's 100 ms BMS_Status cycle really is.

    python -m bms_sim.timing --channel slcan0 --seconds 20

The status frame is sent from a fixed tick grid, so the spacing between
frames (kernel receive timestamps) shows how well the target's sense of time
tracks the host's: on hardware it is 100 ms +- a few us; on an emulator it
reveals dropped timer ticks, scheduling hiccups and -icount effects.
Event-triggered status frames (sent on state changes) are filtered out by
only counting frames whose alive counter advanced by exactly one.
"""
from __future__ import annotations

import argparse
import statistics
import time

import can

from . import protocol as p


def measure(channel: str, seconds: float, interface: str = "socketcan") -> list[float]:
    status = p.msg("BMS_Status")
    bus = can.Bus(interface=interface, channel=channel,
                  can_filters=[{"can_id": status.frame_id, "can_mask": 0x7FF}])
    stamps: list[tuple[float, int]] = []
    end = time.monotonic() + seconds
    try:
        while time.monotonic() < end:
            m = bus.recv(timeout=0.5)
            if m is not None and m.arbitration_id == status.frame_id:
                stamps.append((m.timestamp, m.data[1] & 0x0F))
    finally:
        bus.shutdown()
    return [b[0] - a[0] for a, b in zip(stamps, stamps[1:]) if (b[1] - a[1]) % 16 == 1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--channel", default="vcan0")
    ap.add_argument("--seconds", type=float, default=10.0)
    args = ap.parse_args()

    periods = sorted(dt * 1000.0 for dt in measure(args.channel, args.seconds))
    if len(periods) < 2:
        print("not enough BMS_Status frames - is the firmware running?")
        return 1
    p99 = periods[min(len(periods) - 1, int(0.99 * len(periods)))]
    print(f"{len(periods)} periods on {args.channel}: mean {statistics.mean(periods):.2f} ms, "
          f"stdev {statistics.pstdev(periods):.2f} ms, min {periods[0]:.2f}, p99 {p99:.2f}, max {periods[-1]:.2f}")
    print(f"firmware clock vs host clock: {100.0 / statistics.mean(periods):.4f} "
          f"(1.0 = perfect; < 1 = firmware time runs slow)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
