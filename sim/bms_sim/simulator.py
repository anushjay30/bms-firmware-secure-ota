"""Runs the battery model on a CAN bus and observes the BMS.

Two background threads:
  * TX: steps the model and sends SIM_PackMeas (10 ms) and the cell
    voltage/temperature pages (50 ms), encoded from the DBC.
  * RX: decodes BMS_Status / BMS_FaultEvent, checks their E2E CRC and feeds
    the reported contactor state back into the model.
The public methods are the fault-injection and observation API used by both
the interactive CLI and the pytest scenario harness.
"""
from __future__ import annotations

import os
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Callable, Optional

import can

from . import protocol as p
from .battery import BatteryModel

# Cyclic periods (the DBC's GenMsgCycleTime): ~260 frames/s in total.
# BMS_SIM_PERIOD_SCALE (default 1) stretches them for a host that cannot carry
# that rate - e.g. QEMU nested inside an emulated VM, where the emulated UART
# moves only a few KB/s. Timing budgets derive from CELL_PERIOD_S
# (fwconfig.reaction_budget_s), so they scale with it instead of being
# loosened by hand.
PERIOD_SCALE = float(os.environ.get("BMS_SIM_PERIOD_SCALE", "1"))
PACK_PERIOD_S = 0.010 * PERIOD_SCALE
CELL_PERIOD_S = 0.050 * PERIOD_SCALE


@dataclass
class Status:
    t: float                # local receive time, time.monotonic() (immune to clock steps)
    state: str
    meas_valid: bool
    faults_latched: int
    faults_active: int
    soc_pct: float
    pack_mv: int
    counter: int
    crc_ok: bool


@dataclass
class FaultEvent:
    t: float
    fault: str
    cell: int
    value: int
    faults_latched: int
    counter: int


@dataclass
class _Observed:
    statuses: deque = field(default_factory=lambda: deque(maxlen=2000))
    events: list = field(default_factory=list)


class Simulator:
    def __init__(self, channel: str = "vcan0", interface: str = "socketcan",
                 on_status_change: Optional[Callable[[Status, Optional[Status]], None]] = None,
                 on_fault_event: Optional[Callable[[FaultEvent], None]] = None) -> None:
        self.model = BatteryModel()
        self.mute = False                   # stop sending measurements entirely
        self.tx_dropped = 0                 # cyclic frames the bus could not take
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._obs = _Observed()
        self._stop = threading.Event()
        self._cmd_counter = 0
        self._on_status_change = on_status_change
        self._on_fault_event = on_fault_event
        filters = [{"can_id": p.msg(n).frame_id, "can_mask": 0x7FF} for n in ("BMS_Status", "BMS_FaultEvent")]
        # ThreadSafeBus: TX thread, RX thread and the caller all use the bus.
        self.bus = can.ThreadSafeBus(interface=interface, channel=channel, can_filters=filters)
        self._threads = [threading.Thread(target=self._tx_loop, name="sim-tx", daemon=True),
                         threading.Thread(target=self._rx_loop, name="sim-rx", daemon=True)]

    # ---- lifecycle ----------------------------------------------------------

    def start(self) -> "Simulator":
        for t in self._threads:
            t.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        for t in self._threads:
            t.join(timeout=2)
        self.bus.shutdown()

    def __enter__(self) -> "Simulator":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # ---- fault injection ----------------------------------------------------

    def set_cell_temp(self, cell: int, degc: Optional[float]) -> None:
        """Force cell (1-based) to a temperature; None returns it to the model."""
        with self._lock:
            self.model.cells[cell - 1].temp_override_c = degc

    def set_cell_voltage(self, cell: int, mv: Optional[int]) -> None:
        with self._lock:
            self.model.cells[cell - 1].volt_override_mv = mv

    def set_load(self, amps: float) -> None:
        with self._lock:
            self.model.load_a = amps
            self.model.profile_start = None

    def drive_profile(self, on: bool = True) -> None:
        with self._lock:
            self.model.profile_start = time.monotonic() if on else None

    def spike(self, amps: float, duration_s: float) -> None:
        with self._lock:
            self.model.spike_a = amps
            self.model.spike_until = time.monotonic() + duration_s

    def set_precharge_fault(self, on: bool) -> None:
        with self._lock:
            self.model.precharge_fault = on

    def set_mute(self, on: bool) -> None:
        with self._lock:
            self.mute = on

    def clear_overrides(self) -> None:
        with self._lock:
            self.model.clear_overrides()

    # ---- talking to the BMS ------------------------------------------------

    def send_command(self, command: str | int, corrupt_crc: bool = False) -> float:
        """Send an E2E-protected VCU_Command. Returns the send time (time.monotonic())."""
        with self._lock:
            counter = self._cmd_counter
            self._cmd_counter = (self._cmd_counter + 1) & 0xF
        can_id, data = p.encode_command(command, counter, corrupt_crc)
        return self._send_reliably(can.Message(arbitration_id=can_id, data=data, is_extended_id=False))

    def test_hook(self, hook: str, task: str | int = 0, duration_ms: int = 0, param: int = 0) -> float:
        """Send a BMS_TestHook debug command. Returns the send time (time.monotonic())."""
        m = p.msg("BMS_TestHook")
        task_id = p.TASKS[task] if isinstance(task, str) else int(task)
        data = m.encode({"HookId": p.HOOKS[hook], "TaskId": task_id, "DurationMs": duration_ms, "HookParam": param})
        return self._send_reliably(can.Message(arbitration_id=m.frame_id, data=data, is_extended_id=False))

    def _send_reliably(self, msg: can.Message, timeout: float = 1.0) -> float:
        """Send a one-off frame that must not be lost (command, debug hook).

        A full kernel transmit queue (ENOBUFS, "Transmit buffer full") is
        back-pressure, not a failure: on a slow link such as slcan the queue
        can be momentarily full of measurement frames. Retry for up to
        `timeout`, then give up loudly. Returns the time the frame was handed
        to the kernel, so latencies are measured from the real send.
        """
        deadline = time.monotonic() + timeout
        while True:
            t = time.monotonic()
            try:
                self.bus.send(msg, timeout=0.05)
                return t
            except can.CanOperationError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.002)

    def stall_task(self, task: str | int, duration_ms: int) -> float:
        return self.test_hook("stall", task, duration_ms)

    def report_stacks(self) -> float:
        return self.test_hook("stacks")

    def reboot(self) -> float:
        return self.test_hook("reboot")

    def trigger_fault(self) -> float:
        return self.test_hook("fault")

    def flash_fault(self, power_loss: bool, after_bytes: int) -> float:
        """Arm a flash fault in the BMS: a failed write, or a power cut, after
        `after_bytes` more programmed bytes (QEMU build only)."""
        return self.test_hook("flash_power_loss" if power_loss else "flash_fail_write", param=after_bytes)

    def clear_observations(self) -> None:
        """Forget every status/event seen so far (e.g. after rebooting the BMS)."""
        with self._lock:
            self._obs.statuses.clear()
            self._obs.events.clear()

    # ---- observation -------------------------------------------------------

    @property
    def status(self) -> Optional[Status]:
        with self._lock:
            return self._obs.statuses[-1] if self._obs.statuses else None

    def statuses(self) -> list[Status]:
        with self._lock:
            return list(self._obs.statuses)

    def events(self) -> list[FaultEvent]:
        with self._lock:
            return list(self._obs.events)

    def wait_for_status(self, pred: Callable[[Status], bool], timeout: float, since: float = 0.0) -> Optional[Status]:
        """First status received after `since` (time.monotonic()) that satisfies pred."""
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                for s in self._obs.statuses:
                    if s.t >= since and pred(s):
                        return s
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._cond.wait(remaining)

    def wait_for_event(self, pred: Callable[[FaultEvent], bool], timeout: float, since: float = 0.0) -> Optional[FaultEvent]:
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                for e in self._obs.events:
                    if e.t >= since and pred(e):
                        return e
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._cond.wait(remaining)

    # ---- threads -------------------------------------------------------------

    def _send(self, name: str, signals: dict) -> None:
        """Cyclic measurement frame. If the kernel queue is full right now,
        drop it - like a real sensor; the next cycle carries fresher data -
        and count it, so a saturated link is visible instead of silent."""
        m = p.msg(name)
        try:
            self.bus.send(can.Message(arbitration_id=m.frame_id, data=m.encode(signals), is_extended_id=False))
        except can.CanError:
            self.tx_dropped += 1

    def _tx_loop(self) -> None:
        last = time.monotonic()
        next_pack = next_cells = last
        while not self._stop.is_set():
            now = time.monotonic()
            with self._lock:
                self.model.step(now - last, now)
                mute = self.mute
                current_a = self.model.current_a
                link_mv = int(round(self.model.link_v * 1000.0))
                volts = [self.model.cell_mv(i) for i in range(p.NUM_CELLS)]
                temps = [self.model.cell_temp_c(i) for i in range(p.NUM_CELLS)]
            last = now

            # The schedule always advances, even if a send fails: retrying a
            # failed frame immediately would just spin on a full queue.
            if now >= next_pack:
                if not mute:
                    self._send("SIM_PackMeas", {"PackCurrent": current_a, "LinkVoltage": min(link_mv, 65535)})
                next_pack += PACK_PERIOD_S
            if now >= next_cells:
                if not mute:
                    for page in range(p.PAGES):
                        cells = range(page * p.CELLS_PER_PAGE, (page + 1) * p.CELLS_PER_PAGE)
                        self._send("SIM_CellVoltages", {"CellVoltPage": page,
                                   **{f"CellVoltage_{i + 1:02d}": volts[i] for i in cells}})
                        self._send("SIM_CellTemps", {"CellTempPage": page,
                                   **{f"CellTemp_{i + 1:02d}": round(temps[i], 1) for i in cells}})
                next_cells += CELL_PERIOD_S

            # Fixed-rate schedule; if we fell far behind, re-anchor.
            if now - next_pack > 0.5:
                next_pack = next_cells = now
            time.sleep(max(0.0, min(next_pack, next_cells) - time.monotonic()))

    def _rx_loop(self) -> None:
        status_id = p.msg("BMS_Status").frame_id
        event_id = p.msg("BMS_FaultEvent").frame_id
        while not self._stop.is_set():
            m = self.bus.recv(timeout=0.1)
            if m is None:
                continue
            if m.arbitration_id == status_id:
                self._on_status(m)
            elif m.arbitration_id == event_id:
                self._on_event(m)

    def _on_status(self, m: can.Message) -> None:
        d = p.msg("BMS_Status").decode(m.data, decode_choices=False)
        s = Status(t=time.monotonic(), state=p.STATES.get(d["State"], str(d["State"])), meas_valid=bool(d["MeasValid"]),
                   faults_latched=d["FaultsLatched"], faults_active=d["FaultsActive"], soc_pct=float(d["SoC"]),
                   pack_mv=d["PackVoltage"], counter=d["Status_AliveCounter"],
                   crc_ok=p.e2e_valid(m.arbitration_id, bytes(m.data)))
        with self._cond:
            prev = self._obs.statuses[-1] if self._obs.statuses else None
            self._obs.statuses.append(s)
            if s.crc_ok:
                self.model.bms_state = s.state
            self._cond.notify_all()
        if self._on_status_change and (prev is None or (prev.state, prev.faults_latched, prev.meas_valid) !=
                                       (s.state, s.faults_latched, s.meas_valid)):
            self._on_status_change(s, prev)

    def _on_event(self, m: can.Message) -> None:
        d = p.msg("BMS_FaultEvent").decode(m.data, decode_choices=False)
        e = FaultEvent(t=time.monotonic(), fault=p.FAULTS.get(d["FaultId"], str(d["FaultId"])), cell=d["CellNumber"],
                       value=d["FaultValue"], faults_latched=d["FaultsLatched"], counter=d["EventCounter"])
        with self._cond:
            self._obs.events.append(e)
            self._cond.notify_all()
        if self._on_fault_event:
            self._on_fault_event(e)
