#!/usr/bin/env python3
"""The BMS demo story, driven over CAN (run it through scripts/demo.sh).

  1. v1.0.0 is running from slot A; the battery simulator starts
  2. the VCU closes the contactors (precharge first), a load is drawn
  3. cell 3 overheats -> the BMS opens the contactors within its budget
  4. the cell cools down, the fault is reset
  5. firmware update to v2.0.0 over CAN -> trial boot -> it confirms itself
  6. an update signed with the wrong key -> rejected, v2.0.0 keeps running

usage: demo_story.py --log UART0_LOG [--channel slcan0]
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "sim"))
sys.path.insert(0, str(REPO / "tools"))

import can  # noqa: E402
from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402

import bms_image  # noqa: E402
import bms_ota  # noqa: E402
from bms_sim.simulator import Simulator  # noqa: E402

BOLD, DIM, GREEN, RED, RESET = "\033[1m", "\033[2m", "\033[32m", "\033[31m", "\033[0m"


class Log:
    """Shows the firmware's own log lines (UART0) as the story goes."""

    def __init__(self, path: Path) -> None:
        self.path, self.pos = path, path.stat().st_size

    def show(self, *needles: str) -> None:
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            chunk = f.read().decode(errors="replace")
            self.pos = f.tell()
        for line in chunk.splitlines():
            if not needles or any(n in line for n in needles):
                print(f"   {DIM}| {line}{RESET}")


def step(n: int, text: str) -> None:
    print(f"\n{BOLD}{n}. {text}{RESET}")
    time.sleep(0.8)


def ok(text: str) -> None:
    print(f"   {GREEN}{text}{RESET}")


def wait_status(sim: Simulator, pred, timeout: float, since: float = 0.0, what: str = ""):
    s = sim.wait_for_status(pred, timeout=timeout, since=since)
    if s is None:
        raise SystemExit(f"{RED}timed out waiting for {what}{RESET}")
    return s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", type=Path, required=True)
    ap.add_argument("--channel", default="slcan0")
    args = ap.parse_args()
    log = Log(args.log)

    sim = Simulator(args.channel).start()
    bus = can.Bus(interface="socketcan", channel=args.channel,
                  can_filters=[{"can_id": bms_ota.RSP_ID, "can_mask": 0x7FF}])
    ota = bms_ota.OtaClient(bus, reply_timeout=2.0, retries=10)
    try:
        step(1, "BMS v1 boots from slot A; the battery simulator starts sending cell data")
        info = ota.info()
        wait_status(sim, lambda s: s.meas_valid, 10, what="valid measurements")
        ok(f"BMS reports: {info}")

        step(2, "The VCU asks to close the contactors (precharge first), 40 A load")
        t = sim.send_command("close")
        wait_status(sim, lambda s: s.state == "CLOSED", 3, since=t, what="CLOSED")
        sim.set_load(40.0)
        time.sleep(1.0)
        log.show("SM ")
        ok(f"contactors CLOSED, pack {sim.status.pack_mv / 1000:.1f} V")

        step(3, "Cell 3 overheats to 65 degC")
        t0 = time.monotonic()
        sim.set_cell_temp(3, 65.0)
        s = wait_status(sim, lambda s: s.state == "FAULT", 3, since=t0, what="FAULT")
        time.sleep(0.3)
        log.show("FAULT", "SM ")
        ok(f"BMS opened the contactors {1000 * (s.t - t0):.0f} ms after the fault (budget 300 ms); "
           f"load current is now {sim.model.current_a:.0f} A")

        step(4, "The cell cools down; the VCU resets the fault")
        sim.set_cell_temp(3, None)
        sim.set_load(0.0)
        time.sleep(1.0)
        t = sim.send_command("reset")
        wait_status(sim, lambda s: s.state == "OPEN", 3, since=t, what="OPEN")
        log.show("SM ")
        ok("back to OPEN, no faults latched")

        step(5, "Firmware update to v2.0.0 over CAN (contactors are open, so the BMS accepts it)")
        raw = (REPO / "build/qemu/app_slotB.bin").read_bytes()
        key = serialization.load_pem_private_key((REPO / "keys/dev_ed25519.pem").read_bytes(), password=None)
        v2 = bms_image.sign(raw, key, bms_image.version_from_str("2.0.0"))
        log.show()

        def progress(done: int, total: int, retrans: int) -> None:
            bar = "#" * int(30 * done / total)
            sys.stdout.write(f"\r   [{bar:<30}] {100 * done / total:5.1f}%  retransmissions {retrans}")
            sys.stdout.flush()

        result = ota.update(v2, progress=progress)
        print()
        log.show("OTA", "SM ")
        ok(f"{result.size} bytes in {result.seconds:.1f} s, verified by the BMS (CRC-32 + Ed25519), rebooting")
        trial = ota.wait_for(lambda i: i.running_version == bms_image.version_from_str("2.0.0"), 30)
        log.show("BOOT", "OTA")
        ok(f"BMS reports: {trial}")
        done = ota.wait_for(lambda i: i.confirmed, 30)
        log.show("OTA")
        ok(f"BMS reports: {done}")

        step(6, "Someone tries to install an image signed with a different key")
        forged = bms_image.sign(raw, Ed25519PrivateKey.generate(), bms_image.version_from_str("3.0.0"))
        try:
            ota.update(forged, progress=progress)
            print(f"\n   {RED}accepted?!{RESET}")
            return 1
        except bms_ota.OtaRefused as e:
            print()
            log.show("OTA")
            ok(f"rejected: {e.status_name}; BMS still reports: {ota.info()}")
        print(f"\n{BOLD}Done.{RESET}")
        return 0
    finally:
        bus.shutdown()
        sim.stop()


if __name__ == "__main__":
    sys.exit(main())
