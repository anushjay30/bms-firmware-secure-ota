"""Fixtures for the scenario tests: firmware + simulator on a SocketCAN bus.

The same tests run against either build; only the firmware launcher and the
CAN interface differ:

  BMS_TARGET=posix (default)  build/posix/bms_posix on vcan0, one process per test
  BMS_TARGET=qemu             bootloader + Slot A image in QEMU, CAN via slcan0
                              (scripts/run_qemu.sh: QEMU UART1 -> pty -> slcand);
                              one QEMU session, the firmware is rebooted over
                              CAN (test hook) before every test

Environment:
  BMS_CAN_IF           CAN interface (default vcan0 / slcan0 by target)
  BMS_FW_BIN           POSIX firmware binary
  BMS_BOOT_ELF         QEMU: bootloader ELF
  BMS_SLOT_A           QEMU: image programmed into Slot A at power-on
  BMS_QEMU_GDB_PORT    QEMU: GDB stub port (flash programming), default 1234
  BMS_QEMU_ICOUNT      extra "-icount ..." options for QEMU (see README)
  BMS_SIM_PERIOD_SCALE stretch the simulator's cyclic periods on hosts that
                       cannot carry ~260 frames/s (budgets scale with it)
  BMS_TIMING_SLACK_MS  extra timing allowance for slow hosts (default 0)
"""
from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

from bms_sim import protocol as p

# The update tests use the host tools and the GDB-stub flash programmer.
sys.path.insert(0, str(p.REPO_ROOT / "tools"))
sys.path.insert(0, str(p.REPO_ROOT / "scripts"))

TARGET = os.environ.get("BMS_TARGET", "posix")
if TARGET not in ("posix", "qemu"):
    raise pytest.UsageError(f"BMS_TARGET must be posix or qemu, not {TARGET!r}")
CAN_IF = os.environ.get("BMS_CAN_IF", "slcan0" if TARGET == "qemu" else "vcan0")
FW_BIN = Path(os.environ.get("BMS_FW_BIN", p.REPO_ROOT / "build/posix/bms_posix"))
BOOT_ELF = Path(os.environ.get("BMS_BOOT_ELF", p.REPO_ROOT / "build/qemu/bootloader.elf"))
SLOT_A_IMAGE = Path(os.environ.get("BMS_SLOT_A", p.REPO_ROOT / "build/qemu/app_slotA.signed.bin"))
GDB_PORT = int(os.environ.get("BMS_QEMU_GDB_PORT", "1234"))


class LogFile:
    """Firmware log on disk, read from a byte offset (the current boot)."""

    def __init__(self, path: Path, offset: int = 0) -> None:
        self.path = path
        self.offset = offset

    def log(self) -> str:
        with open(self.path, "rb") as f:
            f.seek(self.offset)
            return f.read().decode(errors="replace")

    def wait_for_log(self, text: str, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if text in self.log():
                return True
            time.sleep(0.02)
        return False


class PosixFirmware(LogFile):
    """The POSIX binary as a child process, stdout captured to a file."""

    def __init__(self, log_path: Path) -> None:
        super().__init__(log_path)
        self._log = open(log_path, "wb")
        self.proc = subprocess.Popen([str(FW_BIN), CAN_IF], stdout=self._log, stderr=subprocess.STDOUT)

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self._log.close()


class QemuSession:
    """QEMU + slcand via scripts/run_qemu.sh, kept up for the whole test run."""

    def __init__(self, log_path: Path) -> None:
        self.log_path = log_path
        self.proc: subprocess.Popen | None = None
        self.needs_restart = False
        self.start()

    def start(self) -> None:
        cmd = ["bash", str(p.REPO_ROOT / "scripts" / "run_qemu.sh"), "--boot", str(BOOT_ELF),
               "--slot-a", str(SLOT_A_IMAGE), "--iface", CAN_IF, "--log", str(self.log_path),
               "--gdb-port", str(GDB_PORT)]
        if os.environ.get("BMS_QEMU_ICOUNT"):
            cmd += ["--icount", os.environ["BMS_QEMU_ICOUNT"]]
        # Own process group, so stop() can signal QEMU, slcand and the script.
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, start_new_session=True)
        output = []
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                break
            output.append(line)
            if line.startswith("READY"):
                # slcan0 is up; also wait for the first boot to finish, so its
                # "BMS ready" can never be mistaken for a later reboot's.
                if LogFile(self.log_path).wait_for_log("BMS ready", timeout=30):
                    self.needs_restart = False
                    return
                output.append("(no 'BMS ready' on UART0)\n")
                break
        self.stop()
        raise RuntimeError("run_qemu.sh did not become ready:\n" + "".join(output))

    def stop(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
        self.proc = None

    def restart(self) -> None:
        self.stop()
        self.start()


class QemuFirmware(LogFile):
    """One boot of the firmware inside the long-running QEMU session."""

    def __init__(self, session: QemuSession, sim) -> None:
        super().__init__(session.log_path, session.log_path.stat().st_size)
        sim.reboot()  # previous test's firmware resets itself (AIRCR.SYSRESETREQ)
        if not self._wait_for_new_boot(timeout=15):
            # Firmware wedged (e.g. a FATAL_HALT build stopped in the fault
            # handler, or the bootloader found no valid image). Restarting
            # QEMU now would also recreate slcan0 under the simulator's open
            # socket, so fail this test and restart before the next one.
            session.needs_restart = True
            print(f"\n----- simulator: {sim.tx_dropped} cyclic frames dropped -----")
            print("----- firmware log (no reboot seen) -----\n" + self.log()[-6000:])
            pytest.fail("firmware did not reboot on request; QEMU will be restarted for the next test")
        sim.clear_observations()  # forget frames from the previous boot

    def _wait_for_new_boot(self, timeout: float) -> bool:
        """Wait for a new boot banner followed by "BMS ready", then move the
        log window to the start of that boot. The banner is printed straight
        from main() before the scheduler starts, so unlike queued log lines it
        cannot be lost in the reset. Everything before our offset belongs to
        earlier boots."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = self.log()
            start = text.find("BMS firmware starting")
            if start >= 0 and text.find("BMS ready", start) >= 0:
                self.offset += start  # ASCII log: chars == bytes
                return True
            time.sleep(0.05)
        return False

    def stop(self) -> None:
        pass  # the next test reboots it


def memory_map() -> dict[str, int]:
    """Addresses from the header the firmware is built with."""
    values = {}
    for line in (p.REPO_ROOT / "port/qemu-mps2/memory_map.h").read_text().splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "#define" and parts[1].startswith("MM_") and parts[2].startswith("0x"):
            values[parts[1]] = int(parts[2], 16)
    return values


class QemuDevice(LogFile):
    """The device in the long-running QEMU session, seen as a whole: flash
    (through the GDB stub, like an SWD probe) and every boot in the log.

    factory_reset() puts it in the state it leaves the factory in: v1 in slot
    A, slot B and the metadata erased - so every update test starts from the
    same place regardless of what the previous one left behind."""

    def __init__(self, session: QemuSession) -> None:
        super().__init__(session.log_path, session.log_path.stat().st_size)
        self.mm = memory_map()

    def factory_reset(self, timeout: float = 30.0) -> None:
        from qemu_flash import GdbStub

        self.offset = self.path.stat().st_size
        with GdbStub(port=GDB_PORT) as stub:  # connecting halts the CPU
            stub.erase(self.mm["MM_META_BASE"], self.mm["MM_META_SIZE"])
            stub.erase(self.mm["MM_SLOT_B_BASE"], 0x40000)  # far more than an image needs
            stub.write(self.mm["MM_SLOT_A_BASE"], SLOT_A_IMAGE.read_bytes())
            stub.monitor("system_reset")
            stub.detach()
        assert self.wait_for_boots(1, timeout), "device did not boot after the factory reset:\n" + self.log()[-4000:]

    def read_meta(self):
        """The metadata record the bootloader will use (newest valid copy)."""
        from boot_meta import newest
        from qemu_flash import GdbStub

        with GdbStub(port=GDB_PORT) as stub:
            copies = [stub.read(self.mm[name], 32) for name in ("MM_META_COPY0", "MM_META_COPY1")]
            stub.detach()
        return newest(*copies)[0]

    def boots(self) -> int:
        """Completed boots since the window started: app banner followed by "BMS ready"."""
        text, count, pos = self.log(), 0, 0
        while (start := text.find("BMS firmware starting", pos)) >= 0:
            ready = text.find("BMS ready", start)
            if ready < 0:
                break
            count, pos = count + 1, ready
        return count

    def wait_for_boots(self, count: int, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.boots() >= count:
                return True
            time.sleep(0.1)
        return False


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    # Make each phase's report available to fixtures (to dump logs on failure).
    outcome = yield
    rep = outcome.get_result()
    setattr(item, f"rep_{rep.when}", rep)


@pytest.fixture(scope="session")
def qemu_session(tmp_path_factory):
    for image in (BOOT_ELF, SLOT_A_IMAGE):
        if not image.exists():
            pytest.skip(f"{image} not built (run: make TARGET=qemu)")
    session = QemuSession(tmp_path_factory.mktemp("qemu") / "uart0.log")
    yield session
    session.stop()


@pytest.fixture
def sim(request):
    if TARGET == "qemu":
        session = request.getfixturevalue("qemu_session")  # brings slcan0 up first
        if session.needs_restart:
            session.restart()  # before the simulator opens its socket on slcan0
    if not Path(f"/sys/class/net/{CAN_IF}").exists():
        pytest.skip(f"{CAN_IF} not present (vcan0: sudo bash scripts/setup_vcan.sh)")
    from bms_sim.simulator import Simulator

    s = Simulator(CAN_IF).start()
    yield s
    s.stop()


@pytest.fixture
def bms(sim, tmp_path, request):
    """A freshly booted firmware, started after the simulator so measurements
    are already flowing when it comes up."""
    if TARGET == "qemu":
        fw = QemuFirmware(request.getfixturevalue("qemu_session"), sim)
    else:
        if not FW_BIN.exists():
            pytest.skip(f"{FW_BIN} not built (run: make)")
        fw = PosixFirmware(tmp_path / "bms.log")
    assert sim.wait_for_status(lambda s: True, timeout=5), "firmware never sent BMS_Status"
    yield fw
    fw.stop()
    # Dump on a failed test body, and on a failed later fixture (e.g. `closed`).
    failed = any(getattr(getattr(request.node, f"rep_{phase}", None), "failed", False) for phase in ("setup", "call"))
    if failed:
        print(f"\n----- simulator: {sim.tx_dropped} cyclic frames dropped by a full TX queue -----")
        print("----- firmware log -----\n" + fw.log()[-8000:])


@pytest.fixture
def device(sim, request):
    """QEMU only: the device freshly factory-reset (v1 in slot A), with the
    simulator running. For the firmware update scenarios."""
    if TARGET != "qemu":
        pytest.skip("firmware updates need the bootloader: QEMU target only")
    dev = QemuDevice(request.getfixturevalue("qemu_session"))
    try:
        dev.factory_reset()
    except Exception:
        request.getfixturevalue("qemu_session").needs_restart = True
        raise
    sim.clear_observations()
    assert sim.wait_for_status(lambda s: s.meas_valid, timeout=10), "measurements never became valid"
    yield dev
    failed = any(getattr(getattr(request.node, f"rep_{phase}", None), "failed", False) for phase in ("setup", "call"))
    if failed:
        print("----- firmware log -----\n" + dev.log()[-12000:])


@pytest.fixture
def closed(bms, sim):
    """Firmware running with contactors CLOSED and a 20 A discharge load."""
    assert sim.wait_for_status(lambda s: s.meas_valid, timeout=3), "measurements never became valid"
    sim.set_load(20.0)
    t = sim.send_command("close")
    assert sim.wait_for_status(lambda s: s.state == "CLOSED", timeout=2, since=t), "contactors did not close"
    return sim
