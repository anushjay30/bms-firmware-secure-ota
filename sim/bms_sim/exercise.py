"""Drive one firmware boot through every major code path, then ask it for
its stack high-water marks - the numbers to size task stacks from.

    python -m bms_sim.exercise --channel slcan0

Watch the firmware log for the "STACK" lines at the end.
"""
from __future__ import annotations

import argparse
import time

from .simulator import Simulator


def run(channel: str, interface: str = "socketcan") -> None:
    with Simulator(channel, interface) as sim:
        sim.set_load(20.0)
        assert sim.wait_for_status(lambda s: s.meas_valid, timeout=5), "no valid measurements"
        steps = [
            ("close contactors", lambda: sim.send_command("close")),
            ("drive cycle", lambda: sim.drive_profile(True)),
            ("overheat cell 3", lambda: sim.set_cell_temp(3, 65.0)),
            ("reset while hot (refused)", lambda: sim.send_command("reset")),
            ("cool cell 3", lambda: sim.set_cell_temp(3, None)),
            ("reset (accepted)", lambda: sim.send_command("reset")),
            ("bad CRC command", lambda: sim.send_command("close", corrupt_crc=True)),
            ("close again", lambda: sim.send_command("close")),
            ("undervoltage cell 2", lambda: sim.set_cell_voltage(2, 2500)),
            ("clear + reset", lambda: (sim.clear_overrides(), sim.send_command("reset"))),
            ("stall monitor task", lambda: sim.stall_task("monitor", 800)),
            ("reset after watchdog", lambda: sim.send_command("reset")),
            ("overcurrent", lambda: (sim.send_command("close"), sim.set_load(350.0))),
            ("sensor loss", lambda: sim.set_mute(True)),
            ("sensor back + reset", lambda: (sim.set_mute(False), sim.set_load(0.0), sim.send_command("reset"))),
        ]
        for name, action in steps:
            print(f"- {name}")
            action()
            time.sleep(1.2)
        sim.report_stacks()
        time.sleep(1.0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--channel", default="vcan0")
    ap.add_argument("--interface", default="socketcan",
                    help="python-can interface; e.g. 'slcan' with --channel socket://127.0.0.1:5555 "
                         "talks straight to QEMU's UART1 without SocketCAN")
    args = ap.parse_args()
    run(args.channel, args.interface)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
