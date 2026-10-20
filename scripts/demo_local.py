#!/usr/bin/env python3
r"""One-command BMS demo that runs anywhere QEMU and Python are installed -
no Linux, no SocketCAN, no second terminal.

It boots the real Cortex-M3 firmware (bootloader + slot A) in QEMU, bridges the
CAN UART to a local TCP port, runs the Python battery simulator against it, and
narrates the story:

    default: boot v1 -> close contactors -> overheat a cell -> contactors open
             -> reset the fault
    --ota:   boot v1 -> update to v2 over CAN -> v2 boots on trial and confirms
             itself -> a forged update is rejected

Usage (from the repo root):
    python scripts/demo_local.py            # the BMS story (fast)
    python scripts/demo_local.py --ota      # the firmware-update story
    python scripts/demo_local.py --qemu PATH-TO-qemu-system-arm

(The two are separate boots: over a local TCP link QEMU's CAN UART takes a
single client, so the update story talks to the BMS directly instead of
through the battery simulator. The "cells stay monitored during an update"
property is covered by the scenario test sim/tests/test_ota.py.)

Needs: qemu-system-arm on PATH (or --qemu / the QEMU env var), and the Python
packages in sim/requirements.txt. Build the images first with
`make TARGET=qemu` (an ARM gcc toolchain); this script never builds.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "sim"))
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "scripts"))

BUILD = REPO / "build" / "qemu"
GDB_PORT, UART_PORT = 1234, 5555
BOLD, DIM, GREEN, RED, RESET = "\033[1m", "\033[2m", "\033[32m", "\033[31m", "\033[0m"


def find_qemu(explicit: str | None) -> str:
    for cand in (explicit, os.environ.get("QEMU"), "qemu-system-arm",
                 r"C:\Users\anush\bms-vm\msys64\ucrt64\bin\qemu-system-arm.exe"):
        if cand and (shutil.which(cand) or Path(cand).exists()):
            return cand
    sys.exit("error: qemu-system-arm not found. Install it, or pass --qemu <path>.")


def need(path: Path) -> Path:
    if not path.exists():
        sys.exit(f"error: {path.relative_to(REPO)} missing - build it first with:  make TARGET=qemu")
    return path


def step(text: str) -> None:
    print(f"\n{BOLD}>> {text}{RESET}", flush=True)
    time.sleep(0.5)


def ok(text: str) -> None:
    print(f"   {GREEN}{text}{RESET}", flush=True)


class Firmware:
    """The firmware log (UART0), shown as it grows."""

    def __init__(self, path: Path) -> None:
        self.path, self.pos = path, 0

    def show(self) -> None:
        text = self.path.read_text(errors="replace")
        for line in text[self.pos:].splitlines():
            if any(k in line for k in ("BOOT", "MAIN", "SM ", "FAULT", "OTA")):
                print(f"   {DIM}fw| {line}{RESET}", flush=True)
        self.pos = len(text)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ota", action="store_true", help="also demo a firmware update over CAN")
    ap.add_argument("--qemu", help="path to qemu-system-arm")
    args = ap.parse_args()

    qemu_bin = find_qemu(args.qemu)
    boot = need(BUILD / "bootloader.elf")
    slot_a = need(BUILD / "app_slotA.signed.bin")

    try:
        import can  # noqa: F401
        from qemu_flash import GdbStub
        from bms_sim.simulator import Simulator
        if args.ota:
            import bms_image  # noqa: F401
            import bms_ota  # noqa: F401
            from cryptography.hazmat.primitives import serialization  # noqa: F401
    except ModuleNotFoundError as e:
        sys.exit(f"error: missing Python package ({e.name}). Install them with:\n"
                 f"    {Path(sys.executable).name} -m pip install -r sim/requirements.txt")

    work = Path(os.environ.get("TEMP", "/tmp")) / "bms_demo"
    work.mkdir(exist_ok=True)
    log = work / "uart0.log"
    log.write_text("")
    fw = Firmware(log)

    print(f"{BOLD}BMS demo{RESET}  (Cortex-M3 firmware in QEMU + Python battery simulator)")
    print(f"{DIM}qemu: {qemu_bin}{RESET}")

    # Start QEMU halted: UART0 -> log file, UART1 (CAN) -> TCP, GDB stub for
    # programming the slots like an SWD probe (see scripts/run_qemu.sh).
    qemu = subprocess.Popen(
        [qemu_bin, "-M", "mps2-an385", "-cpu", "cortex-m3", "-kernel", str(boot),
         "-S", "-gdb", f"tcp:127.0.0.1:{GDB_PORT}", "-display", "none", "-monitor", "none",
         "-serial", f"file:{log}", "-serial", f"tcp:127.0.0.1:{UART_PORT},server=on,wait=off"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    sim = None
    try:
        step("Programming flash and starting the firmware")
        stub = GdbStub(port=GDB_PORT)
        stub.erase(0x10000, 0x210000)                 # metadata + both slots
        stub.write(0x20000, slot_a.read_bytes())       # slot A
        stub.detach()                                  # QEMU runs
        deadline = time.monotonic() + 30
        while "BMS ready" not in log.read_text(errors="replace"):
            if time.monotonic() > deadline or qemu.poll() is not None:
                print(log.read_text(errors="replace")[-2000:])
                sys.exit("firmware did not reach 'BMS ready'")
            time.sleep(0.1)
        fw.show()
        ok("firmware booted: bootloader verified the Ed25519 signature and started slot A")

        if args.ota:
            demo_ota(fw)
        else:
            sim = demo_bms(fw)

        print(f"\n{BOLD}Demo complete.{RESET}")
        return 0
    finally:
        if sim is not None:
            sim.stop()
        qemu.kill()
        qemu.wait()


def demo_bms(fw: Firmware):
    from bms_sim.simulator import Simulator

    sim = Simulator(f"socket://127.0.0.1:{UART_PORT}", interface="slcan").start()
    sim.wait_for_status(lambda s: s.meas_valid, timeout=10)
    ok(f"simulator connected; BMS reports {sim.status.state}, "
       f"SoC {sim.status.soc_pct:.0f}%, pack {sim.status.pack_mv/1000:.1f} V")

    step("VCU closes the contactors (precharge first) and draws 40 A")
    t = sim.send_command("close")
    sim.wait_for_status(lambda s: s.state == "CLOSED", timeout=3, since=t)
    sim.set_load(40.0)
    time.sleep(0.6)
    fw.show()
    ok(f"contactors CLOSED, {sim.model.current_a:.0f} A flowing")

    step("Cell 3 overheats to 65 C")
    sim.set_cell_temp(3, 65.0)
    sim.wait_for_status(lambda s: s.state == "FAULT", timeout=3)
    sim.wait_for_event(lambda e: e.fault == "CELL_OT", timeout=1)
    time.sleep(0.4)
    fw.show()
    ok(f"BMS latched CELL_OT and opened the contactors; load is now {sim.model.current_a:.0f} A")

    step("Cell cools down; VCU resets the fault")
    sim.set_cell_temp(3, None)
    sim.set_load(0.0)
    time.sleep(1.0)
    t = sim.send_command("reset")
    sim.wait_for_status(lambda s: s.state == "OPEN", timeout=3, since=t)
    fw.show()
    ok(f"back to OPEN, faults latched = {sim.status.faults_latched}")
    return sim


def demo_ota(fw: Firmware) -> None:
    import can
    import bms_image
    import bms_ota
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    key_path = REPO / "keys" / "dev_ed25519.pem"
    if not key_path.exists():
        sys.exit("error: keys/dev_ed25519.pem missing - run 'make TARGET=qemu' once to generate it")
    key = serialization.load_pem_private_key(key_path.read_bytes(), password=None)
    raw_b = need(BUILD / "app_slotB.bin").read_bytes()
    v2 = bms_image.sign(raw_b, key, bms_image.version_from_str("2.0.0"))

    bus = can.Bus(interface="slcan", channel=f"socket://127.0.0.1:{UART_PORT}",
                  can_filters=[{"can_id": bms_ota.RSP_ID, "can_mask": 0x7FF}])
    ota = bms_ota.OtaClient(bus, reply_timeout=2.0, retries=10)

    step("Firmware update to v2.0.0 over CAN (allowed: contactors are open)")

    def progress(done: int, total: int, retrans: int) -> None:
        bar = "#" * int(28 * done / total)
        print(f"\r   [{bar:<28}] {100*done/total:5.1f}%  retransmissions {retrans}", end="", flush=True)

    result = ota.update(v2, progress=progress)
    print()
    fw.show()
    ok(f"{result.size} bytes in {result.seconds:.1f} s, verified by the BMS (CRC-32 + Ed25519); rebooting")
    trial = ota.wait_for(lambda i: i.running_version == bms_image.version_from_str("2.0.0"), timeout=30)
    ok(f"new image is running on trial: {trial}")
    confirmed = ota.wait_for(lambda i: i.confirmed, timeout=30)
    fw.show()
    ok(f"v2.0.0 confirmed itself after a health check: {confirmed}")

    step("Someone tries to install an image signed with the wrong key")
    # v2 now runs from slot B, so the inactive slot is A: forge the image the
    # updater will actually target (an image for the wrong slot would be
    # refused as BadSlot before the signature is even checked).
    inactive_raw = need(BUILD / f"app_slot{'AB'[ota.info().running_slot ^ 1]}.bin").read_bytes()
    forged = bms_image.sign(inactive_raw, Ed25519PrivateKey.generate(), bms_image.version_from_str("3.0.0"))
    try:
        ota.update(forged, progress=progress)
        print(f"\n   {RED}accepted?! (should not happen){RESET}")
    except bms_ota.OtaRefused as e:
        print()
        fw.show()
        ok(f"rejected: {e.status_name}; the BMS keeps running v{bms_image.version_to_str(ota.info().running_version)}")
    finally:
        bus.shutdown()


if __name__ == "__main__":
    sys.exit(main())
