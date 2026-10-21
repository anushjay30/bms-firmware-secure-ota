#!/usr/bin/env python3
"""Scripted, captioned BMS demo for the dashboard - the source of the demo video.

Boots the real Cortex-M3 firmware in QEMU, owns one CAN connection (the battery
simulator, the OTA client and the dashboard feed all share it), and drives the
story end to end while a caption banner explains each step. Measured latencies
are real: QEMU does not run in real time on every host, so the firmware's clock
rate is measured live from its 100 ms status cadence and reaction times are
reported in firmware time.

    python scripts/demo_video.py --no-record     # run the story, no recording
    python scripts/demo_video.py                 # record it (Playwright) -> webm
    make demo-video                              # record + ffmpeg -> docs/demo.{mp4,gif}

Steps: boot v1.0 -> close + load -> cell overheats -> contactors open -> reset
-> OTA to v1.1 (confirms) -> forged update rejected -> buggy v1.2 rolls back.
"""
from __future__ import annotations

import argparse
import os
import shutil
import statistics
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "sim"))
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(REPO))

BUILD = REPO / "build" / "qemu"
GDB_PORT, UART_PORT, DASH_PORT = 1234, 5555, 8000


def find_qemu(explicit: str | None) -> str:
    for cand in (explicit, os.environ.get("QEMU"), "qemu-system-arm",
                 r"C:\Users\anush\bms-vm\msys64\ucrt64\bin\qemu-system-arm.exe"):
        if cand and (shutil.which(cand) or Path(cand).exists()):
            return cand
    sys.exit("error: qemu-system-arm not found (pass --qemu <path>).")


def need(path: Path) -> Path:
    if not path.exists():
        sys.exit(f"error: {path} missing - build it with  make TARGET=qemu")
    return path


class Demo:
    def __init__(self, qemu_bin: str) -> None:
        import can
        from bms_sim import protocol as p
        from bms_sim.battery import BatteryModel
        from dashboard import server

        self.can, self.p, self.server = can, p, server
        self.model = BatteryModel()
        self.hub = server.hub
        self.qemu_bin = qemu_bin
        self.log = Path(os.environ.get("TEMP", "/tmp")) / "bms_demo_video" / "uart0.log"
        self.log.parent.mkdir(exist_ok=True)
        self.log.write_text("")

        self.bus = None
        self.qemu = None
        self._stop = threading.Event()
        self._status = None            # latest decoded BMS_Status dict
        self._status_wall = deque(maxlen=40)  # (wall, firmware_counter) for ratio
        self._ota_inbox = None
        self._mute = False
        self._pack_period = 0.010
        self._cell_period = 0.050
        self._fault_log: list = []
        self._uart_lines: deque = deque(maxlen=12)
        self._ratio = 0.8              # firmware/wall, measured after boot
        self.rec_t0: float | None = None   # recorder start wall time
        self.markers: list = []            # (label, wall offset from recorder start)
        self._t0 = time.monotonic()        # demo start, for relative timestamps
        self._ids = {n: p.msg(n).frame_id for n in
                     ("BMS_Status", "BMS_FaultEvent", "SIM_PackMeas", "SIM_CellVoltages", "SIM_CellTemps")}
        import queue
        self._ota_inbox = queue.Queue()

    # ---- lifecycle ----------------------------------------------------------

    def start(self) -> None:
        self.server.serve_in_thread(port=DASH_PORT)
        self._boot()
        # ThreadSafeBus: the TX thread, the RX thread and the OTA client (main
        # thread) all use this one connection - QEMU's CAN UART over TCP takes
        # a single client, so everything shares it.
        self.bus = self.can.ThreadSafeBus(interface="slcan", channel=f"socket://127.0.0.1:{UART_PORT}")
        threading.Thread(target=self._tx_loop, name="demo-tx", daemon=True).start()
        threading.Thread(target=self._rx_loop, name="demo-rx", daemon=True).start()
        threading.Thread(target=self._uart_loop, name="demo-uart", daemon=True).start()
        self._measure_ratio()

    def _boot(self) -> None:
        from qemu_flash import GdbStub
        self.qemu = subprocess.Popen(
            [self.qemu_bin, "-M", "mps2-an385", "-cpu", "cortex-m3", "-kernel", str(need(BUILD / "bootloader.elf")),
             "-S", "-gdb", f"tcp:127.0.0.1:{GDB_PORT}", "-display", "none", "-monitor", "none",
             "-serial", f"file:{self.log}", "-serial", f"tcp:127.0.0.1:{UART_PORT},server=on,wait=off"],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        stub = GdbStub(port=GDB_PORT)
        stub.erase(0x10000, 0x210000)
        stub.write(0x20000, need(BUILD / "app_slotA.signed.bin").read_bytes())
        stub.detach()
        deadline = time.monotonic() + 30
        while "BMS ready" not in self.log.read_text(errors="replace"):
            if time.monotonic() > deadline or self.qemu.poll() is not None:
                sys.exit("firmware did not boot:\n" + self.log.read_text(errors="replace")[-2000:])
            time.sleep(0.05)

    def shutdown(self) -> None:
        self._stop.set()
        time.sleep(0.2)
        if self.bus is not None:
            try:
                self.bus.shutdown()
            except Exception:
                pass
        if self.qemu is not None:
            self.qemu.kill()
            self.qemu.wait()

    # ---- CAN threads --------------------------------------------------------

    def _tx_loop(self) -> None:
        last = time.monotonic()
        next_pack = next_cells = next_pub = last
        while not self._stop.is_set():
            now = time.monotonic()
            self.model.step(now - last, now)
            last = now
            volts = [self.model.cell_mv(i) for i in range(12)]
            temps = [self.model.cell_temp_c(i) for i in range(12)]
            if not self._mute and now >= next_pack:
                self._send("SIM_PackMeas", {"PackCurrent": self.model.current_a,
                           "LinkVoltage": min(int(self.model.link_v * 1000), 65535)})
                next_pack = now + self._pack_period
            if not self._mute and now >= next_cells:
                for page in range(4):
                    self._send("SIM_CellVoltages", {"CellVoltPage": page,
                               **{f"CellVoltage_{page*3+i+1:02d}": volts[page*3+i] for i in range(3)}})
                    self._send("SIM_CellTemps", {"CellTempPage": page,
                               **{f"CellTemp_{page*3+i+1:02d}": round(temps[page*3+i], 1) for i in range(3)}})
                next_cells = now + self._cell_period
            # feed the dashboard with ground-truth cell data, throttled to ~30 Hz
            if now >= next_pub:
                self.hub.publish(cells_mv=[int(v) for v in volts], cells_temp=[round(t, 1) for t in temps],
                                 pack_current_a=round(self.model.current_a, 1))
                next_pub = now + 0.033
            time.sleep(max(0.0, min(next_pack, next_cells) - time.monotonic()))

    def _send(self, name: str, signals: dict) -> None:
        m = self.p.msg(name)
        try:
            self.bus.send(self.can.Message(arbitration_id=m.frame_id, data=m.encode(signals), is_extended_id=False),
                          timeout=0.2)
        except self.can.CanError:
            pass

    def _rx_loop(self) -> None:
        from bms_ota import RSP_ID
        while not self._stop.is_set():
            m = self.bus.recv(timeout=0.2)
            if m is None:
                continue
            if m.arbitration_id == self._ids["BMS_Status"]:
                self._on_status(m)
            elif m.arbitration_id == self._ids["BMS_FaultEvent"]:
                self._on_fault(m)
            elif m.arbitration_id == RSP_ID:
                self._ota_inbox.put(m)

    def _on_status(self, m) -> None:
        d = self.p.msg("BMS_Status").decode(m.data, decode_choices=False)
        self._status = d
        self.model.bms_state = self.p.STATES.get(d["State"], "OPEN")
        self._status_wall.append(time.monotonic())
        self.hub.publish(state=self.p.STATES.get(d["State"], "?"), soc_pct=float(d["SoC"]),
                         pack_mv=int(d["PackVoltage"]), meas_valid=bool(d["MeasValid"]),
                         faults_latched=int(d["FaultsLatched"]), faults_active=int(d["FaultsActive"]),
                         t=time.monotonic())

    def _on_fault(self, m) -> None:
        d = self.p.msg("BMS_FaultEvent").decode(m.data, decode_choices=False)
        name = self.p.FAULTS.get(d["FaultId"], str(d["FaultId"]))
        cell = f" (cell {d['CellNumber']})" if d["CellNumber"] else ""
        self._fault_log.append({"t": self._clock(), "text": f"{name}{cell}", "kind": "bad"})
        self.hub.publish(fault_log=self._fault_log[-7:])

    def _uart_loop(self) -> None:
        pos = 0
        while not self._stop.is_set():
            try:
                with open(self.log, "rb") as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos = f.tell()
            except OSError:
                time.sleep(0.1)
                continue
            if chunk:
                for line in chunk.decode(errors="replace").splitlines():
                    if line.strip():
                        self._uart_lines.append(line)
                self.hub.publish(uart=list(self._uart_lines))
            time.sleep(0.12)

    def _clock(self) -> str:
        e = time.monotonic() - self._t0
        return f"{int(e // 60):02d}:{int(e % 60):02d}"

    # ---- timing -------------------------------------------------------------

    def _measure_ratio(self) -> None:
        """firmware/wall rate from the 100 ms status cadence (median of a window)."""
        self._status_wall.clear()
        time.sleep(3.0)
        ts = list(self._status_wall)
        periods = [1000 * (b - a) for a, b in zip(ts, ts[1:]) if 0 < (b - a) < 1]
        if len(periods) >= 5:
            self._ratio = 100.0 / statistics.median(periods)
        self._ratio = min(max(self._ratio, 0.05), 1.0)

    def fw_sleep(self, firmware_seconds: float) -> None:
        """Sleep for a given amount of firmware time (so caption holds are
        constant in the final, speed-normalised video)."""
        time.sleep(firmware_seconds / self._ratio)

    # ---- actions ------------------------------------------------------------

    def caption(self, text: str, kind: str = "info") -> None:
        self.hub.publish(caption={"text": text, "kind": kind})
        print(f"[{kind:5s}] {text}")

    def marker(self, label: str) -> None:
        if self.rec_t0 is not None:
            self.markers.append((label, time.monotonic() - self.rec_t0))

    def _command(self, name: str) -> float:
        from bms_sim import protocol as p
        counter = getattr(self, "_cmd_ctr", 0)
        self._cmd_ctr = (counter + 1) & 0xF
        can_id, data = p.encode_command(name, counter)
        t = time.monotonic()
        self.bus.send(self.can.Message(arbitration_id=can_id, data=data, is_extended_id=False), timeout=0.5)
        return t

    def wait_state(self, state: str, timeout: float = 8.0) -> bool:
        deadline = time.monotonic() + timeout / self._ratio
        while time.monotonic() < deadline:
            if self._status and self.p.STATES.get(self._status["State"]) == state:
                return True
            time.sleep(0.02)
        return False

    def set_load(self, amps: float) -> None:
        self.model.load_a = amps
        self.model.profile_start = None

    def heat_cell(self, cell: int, degc: float | None) -> None:
        self.model.cells[cell - 1].temp_override_c = degc

    def quiet_measurements(self, on: bool) -> None:
        """During a transfer, slow the measurement frames right down (still
        well inside the 300 ms freshness window) so OTA data and the ACKs are
        not stuck behind them in the one-byte UART FIFO - it is the frames
        queued ahead of each ACK, not raw bandwidth, that gates the transfer."""
        self._pack_period = 0.030 if on else 0.010
        self._cell_period = 0.150 if on else 0.050

    def ota_panel(self, **partial) -> None:
        cur = dict(self.hub.snapshot()[1].get("ota", {}))
        cur.update(partial)
        self.hub.publish(ota=cur)

    # ---- OTA ----------------------------------------------------------------

    def ota_client(self, max_rate: float | None = 500.0):
        # Paced to ~500 frames/s: a 44 KB image in ~13 s of firmware time, while
        # leaving room for the ~260 frames/s of cell measurements on the shared
        # serial link and small gaps for the lowest-priority logger task to run.
        import bms_ota
        return bms_ota.OtaClient(self.bus, reply_timeout=2.0, retries=12,
                                 max_frame_rate=max_rate, inbox=self._ota_inbox)

    def info(self):
        return self.ota_client().info()


# ---- image helpers ----------------------------------------------------------

def sign_for(slot: str, version: str, key=None):
    import bms_image
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    if key == "wrong":
        key = Ed25519PrivateKey.generate()
    elif key is None:
        key = serialization.load_pem_private_key(need(REPO / "keys/dev_ed25519.pem").read_bytes(), password=None)
    raw = need(BUILD / f"app_slot{slot}.bin").read_bytes()
    return bms_image.sign(raw, key, bms_image.version_from_str(version))


def noconfirm_image(slot: str, version: str):
    import bms_image
    from cryptography.hazmat.primitives import serialization
    key = serialization.load_pem_private_key((REPO / "keys/dev_ed25519.pem").read_bytes(), password=None)
    raw = need(BUILD.parent / "qemu-demo-nc" / f"app_slot{slot}.bin").read_bytes()
    return bms_image.sign(raw, key, bms_image.version_from_str(version))


# ---- the story --------------------------------------------------------------

def run_scenario(d: Demo) -> None:
    import bms_ota

    # 2. boot
    d.marker("boot")
    d.ota_panel(active_slot="A", version="1.0.0", status="confirmed", pending_slot=None, pending=None,
                progress=None, note="")
    d.caption("Bootloader checked the firmware's Ed25519 signature and started <b>v1.0</b> from Slot A.", "ok")
    d.fw_sleep(3.0)

    # 3. close + load, SoC drifts down
    d.caption("The vehicle asks to connect the pack. The BMS pre-charges, then closes the contactors.", "info")
    d._command("close")
    d.wait_state("CLOSED", timeout=6)
    d.set_load(55.0)
    d.caption("Pack online — delivering current under load. State of charge is tracked by coulomb counting.", "ok")
    d.fw_sleep(3.0)
    d.set_load(85.0)
    d.fw_sleep(2.5)

    # 4. overtemp on cell 7 -> fault, measured latency
    d.marker("fault")
    d.caption("A cell starts to overheat&hellip; cell 7 is climbing toward the 60 °C limit.", "warn")
    d.heat_cell(7, 66.0)
    t0 = time.monotonic()
    got = d.wait_state("FAULT", timeout=5)
    latency_ms = (time.monotonic() - t0) * d._ratio * 1000.0 if got else 0.0
    d.fw_sleep(0.8)
    if got:
        d.caption(f"Fault detected and contactors opened in <b>{latency_ms:.0f} ms</b> — the pack is isolated, "
                  f"current is now 0 A.", "bad")
    else:
        d.caption("Overtemperature fault — contactors opened to isolate the pack.", "bad")
    d.fw_sleep(3.5)

    # 5. cool + reset
    d.caption("The cell cools down. The vehicle clears the fault once the condition is gone.", "info")
    d.heat_cell(7, None)
    d.set_load(0.0)
    d.fw_sleep(2.0)
    d._command("reset")
    d.wait_state("OPEN", timeout=5)
    d.caption("Fault reset — the pack is safe and back to standby (contactors open).", "ok")
    d.fw_sleep(2.5)

    # 6. OTA to v1.1
    d.marker("ota")
    d.caption("Time for a firmware update. A signed <b>v1.1</b> image is sent to the spare slot over CAN.", "ota")
    v11 = sign_for("B", "1.1.0")
    ota = d.ota_client()

    def progress(done, total, retrans):
        d.ota_panel(progress=done / total, pending_slot="B", pending="1.1.0", note="receiving v1.1 → Slot B")

    d.ota_panel(progress=0.0, pending_slot="B", pending="1.1.0", note="receiving v1.1 → Slot B")
    d.quiet_measurements(True)
    try:
        result = ota.update(v11, progress=progress)
    finally:
        d.quiet_measurements(False)
    d.ota_panel(progress=None, note=f"v1.1 verified in {result.seconds:.0f}s, rebooting")
    d.caption("Transfer complete. The BMS verified CRC-32 <b>and</b> the Ed25519 signature, then rebooted.", "ota")
    trial = ota.wait_for(lambda i: i.running_version == 0x010100, timeout=30)
    if trial:
        d.ota_panel(active_slot="B", version="1.1.0", status="trial", pending_slot=None, pending=None,
                    note="on trial — must pass a health check")
        d.caption("<b>v1.1</b> is now running — on trial. It must stay healthy before it is trusted.", "warn")
    d.fw_sleep(2.5)
    confirmed = ota.wait_for(lambda i: i.confirmed, timeout=30)
    if confirmed:
        d.ota_panel(status="confirmed", note="confirmed")
        d.caption("Health check passed — <b>v1.1</b> is confirmed. The rollback safety net is released.", "ok")
    d.fw_sleep(3.0)

    # 7. attack: wrong-key image
    d.marker("attack")
    d.caption("Now an attack: someone sends an image signed with the <b>wrong key</b>.", "warn")
    forged = sign_for("A", "1.2.0", key="wrong")
    try:
        d.ota_panel(progress=0.0, pending_slot="A", pending="?", note="receiving unknown image → Slot A")

        def p2(done, total, retrans):
            d.ota_panel(progress=done / total)
        d.quiet_measurements(True)
        try:
            d.ota_client().update(forged, progress=p2)
        finally:
            d.quiet_measurements(False)
        d.caption("The imposter image was accepted?! (should not happen)", "bad")
    except bms_ota.OtaRefused as e:
        d.ota_panel(progress=None, pending_slot=None, pending=None, note="rejected: bad signature")
        d.caption(f"<b>Rejected</b> — {e.status_name}. v1.1 keeps running, untouched.", "bad")
    d.fw_sleep(3.0)

    # 8. buggy v1.2 that never confirms -> rollback
    d.marker("rollback")
    d.caption("Finally, a buggy <b>v1.2</b> is installed — but it never confirms itself after booting.", "warn")
    v12 = noconfirm_image("A", "1.2.0")
    ota2 = d.ota_client()
    d.ota_panel(progress=0.0, pending_slot="A", pending="1.2.0", note="receiving v1.2 → Slot A")
    d.quiet_measurements(True)
    try:
        ota2.update(v12, progress=lambda done, total, r: d.ota_panel(progress=done / total))
    finally:
        d.quiet_measurements(False)
    d.ota_panel(progress=None, note="v1.2 staged — rebooting on trial")
    for attempt in range(1, bms_ota_max_trials() + 1):
        trial = ota2.wait_for(lambda i: i.running_version == 0x010200 and i.boot_attempt == attempt, timeout=25)
        d.ota_panel(active_slot="A", version="1.2.0", status="trial", pending_slot=None, pending=None,
                    note=f"trial boot {attempt} of {bms_ota_max_trials()} — not confirming")
        d.caption(f"<b>v1.2</b> boots (attempt {attempt} of {bms_ota_max_trials()}) but does not confirm. "
                  f"The bootloader is counting.", "warn")
        d.fw_sleep(1.6)
        if attempt < bms_ota_max_trials():
            d._reboot_over_can()
    d._reboot_over_can()
    back = ota2.wait_for(lambda i: i.running_version == 0x010100 and i.confirmed, timeout=30)
    if back:
        d.ota_panel(active_slot="B", version="1.1.0", status="confirmed", pending_slot=None, pending=None,
                    note="rolled back to v1.1")
        d.caption("Automatic <b>rollback</b> — after 3 unconfirmed boots the BMS reverted to the trusted v1.1.", "ok")
    d.fw_sleep(3.5)
    d.caption("Signed, fail-safe firmware updates over CAN — with the pack monitored the whole time.", "ok")
    d.fw_sleep(3.0)


def bms_ota_max_trials() -> int:
    import boot_meta
    return boot_meta.MAX_TRIAL_BOOTS


def _reboot_hook(d: Demo) -> None:
    # BMS_TestHook Reboot, sent as a raw frame on the shared bus.
    d.bus.send(d.can.Message(arbitration_id=0x6A0, data=bytes([3, 0, 0, 0, 0, 0, 0, 0]), is_extended_id=False),
               timeout=0.5)


Demo._reboot_over_can = _reboot_hook


def _fine_timers() -> None:
    """Windows' default sleep granularity is ~15 ms, which turns frame pacing
    into coarse bursts; ask for 1 ms so pacing and caption holds are accurate."""
    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.winmm.timeBeginPeriod(1)
        except Exception:
            pass


def main() -> int:
    _fine_timers()
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--no-record", action="store_true", help="run the story without recording")
    ap.add_argument("--qemu", help="path to qemu-system-arm")
    ap.add_argument("--out", type=Path, default=Path(os.environ.get("TEMP", "/tmp")) / "bms_demo_video" / "raw.webm")
    args = ap.parse_args()

    d = Demo(find_qemu(args.qemu))
    recorder = None
    recording_json = args.out.parent / "recording.json"
    try:
        d.start()
        print(f"dashboard: http://127.0.0.1:{DASH_PORT}  (firmware/wall ratio {d._ratio:.2f})")
        if not args.no_record:
            from record_dashboard import Recorder
            recorder = Recorder(f"http://127.0.0.1:{DASH_PORT}/", args.out.parent / "frames", target_fps=12)
            recorder.start()
            d.rec_t0 = recorder.start_wall
            time.sleep(1.2)
        d.marker("scenario_start")
        t0 = time.monotonic()
        run_scenario(d)
        wall = time.monotonic() - t0
        d.marker("scenario_end")
        print(f"scenario done in {wall:.0f}s wall (~{wall * d._ratio:.0f}s firmware time)")
        if recorder is not None:
            time.sleep(0.8)  # let the last caption sit a beat
            info = recorder.stop()
            info.update(ratio=d._ratio, markers=d.markers)
            import json
            recording_json.write_text(json.dumps(info, indent=2))
            print(f"recording: {info['count']} frames, {info['wall_s']:.0f}s wall, {info['capture_fps']:.1f} fps")
    finally:
        d.shutdown()  # stop QEMU + the dashboard server before post-processing

    # Post-process in the quiet process (no live server / QEMU / Playwright).
    if recorder is not None:
        from postprocess_video import postprocess
        print("post-processing with ffmpeg ...")
        outputs = postprocess(recording_json)
        for name, (path, size) in outputs.items():
            print(f"  {name:10s} {path}  ({size/1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

