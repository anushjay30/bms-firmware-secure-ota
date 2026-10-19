"""End-to-end scenarios: real firmware + simulated battery on SocketCAN.

Runs against the POSIX build on vcan0 or the Cortex-M3 build in QEMU on
slcan0 (see conftest.py). Each test gets a freshly booted firmware. Timing
assertions use budgets derived from bms_config.h (see bms_sim/fwconfig.py);
latencies are measured on the monotonic clock, from the moment the fault is
injected to the moment the BMS frame reporting it is received. (Never the
wall clock: NTP can step it in the middle of a measurement.)
"""
from __future__ import annotations

import time

import pytest

from bms_sim import fwconfig
from bms_sim import protocol as p

pytestmark = pytest.mark.scenario
CFG = fwconfig.load()
SLACK_S = fwconfig.slack_ms() / 1000.0


def _report(name: str, latency_s: float, budget_s: float) -> None:
    print(f"\n{name}: {latency_s * 1000:.0f} ms (budget {budget_s * 1000:.0f} ms)")


def _assert_fault_reaction(sim, t0: float, fault: str, budget_s: float, cell: int | None = None):
    ev = sim.wait_for_event(lambda e: e.fault == fault, timeout=budget_s + 2.0, since=t0)
    assert ev is not None, f"no {fault} event"
    if cell is not None:
        assert ev.cell == cell
    st = sim.wait_for_status(lambda s: s.state == "FAULT", timeout=1.0, since=t0)
    assert st is not None, "BMS never reported FAULT"
    assert st.faults_latched & (1 << p.FAULT_IDS[fault])
    latency = st.t - t0
    _report(f"{fault} -> FAULT", latency, budget_s)
    assert latency <= budget_s, f"{fault} reaction {latency * 1000:.0f} ms > budget {budget_s * 1000:.0f} ms"
    return ev, st


def _assert_current_stops(sim) -> None:
    # FAULT means every relay is open, so the simulated load is disconnected.
    deadline = time.monotonic() + 0.5
    while time.monotonic() < deadline and sim.model.current_a != 0.0:
        time.sleep(0.01)
    assert sim.model.current_a == 0.0


# ---------------------------------------------------------------------------


def test_boots_open_with_cyclic_e2e_protected_status(bms, sim):
    time.sleep(1.2)
    st = sim.statuses()[-8:]
    assert len(st) == 8
    assert all(s.crc_ok for s in st), "BMS_Status CRC invalid"
    assert all(s.state == "OPEN" for s in st)
    counters = [s.counter for s in st]
    assert all((b - a) % 16 == 1 for a, b in zip(counters, counters[1:])), counters
    nominal = CFG["BMS_STATUS_PERIOD_MS"] / 1000.0
    periods = [b.t - a.t for a, b in zip(st, st[1:])]
    assert all(abs(dt - nominal) <= 0.03 + SLACK_S for dt in periods), periods
    assert st[-1].soc_pct == pytest.approx(CFG["BMS_SOC_INITIAL_PCT_X100"] / 100.0, abs=0.05)


def test_close_goes_through_precharge_then_closed(bms, sim):
    assert sim.wait_for_status(lambda s: s.meas_valid, timeout=3)
    t0 = sim.send_command("close")
    assert sim.wait_for_status(lambda s: s.state == "PRECHARGE", timeout=1, since=t0)
    closed = sim.wait_for_status(lambda s: s.state == "CLOSED", timeout=2, since=t0)
    assert closed is not None
    _report("precharge", closed.t - t0, CFG["BMS_PRECHARGE_TIMEOUT_MS"] / 1000.0)
    assert closed.t - t0 < CFG["BMS_PRECHARGE_TIMEOUT_MS"] / 1000.0
    assert sim.model.link_v >= 0.95 * sim.model.pack_voltage_v()


def test_overtemp_cell3_opens_contactors_within_budget(closed):
    """Cell 3 at 65 degC -> FAULT (all contactors open) within ~300 ms."""
    budget = fwconfig.reaction_budget_s(CFG["BMS_DEBOUNCE_OT_SET"])
    t0 = time.monotonic()
    closed.set_cell_temp(3, 65.0)
    ev, _ = _assert_fault_reaction(closed, t0, "CELL_OT", budget, cell=3)
    assert ev.value == 650  # 0.1 degC units
    _assert_current_stops(closed)


def test_undervoltage_cell2(closed):
    t0 = time.monotonic()
    closed.set_cell_voltage(2, 2500)
    ev, _ = _assert_fault_reaction(closed, t0, "CELL_UV", fwconfig.reaction_budget_s(CFG["BMS_DEBOUNCE_UV_SET"]), cell=2)
    assert ev.value == 2500


def test_overvoltage_cell7(closed):
    t0 = time.monotonic()
    closed.set_cell_voltage(7, 4300)
    _assert_fault_reaction(closed, t0, "CELL_OV", fwconfig.reaction_budget_s(CFG["BMS_DEBOUNCE_OV_SET"]), cell=7)


def test_short_current_spike_is_debounced(closed):
    """30 ms at 400 A is shorter than one monitor period, so at most one
    sample can see it - below the debounce count. No trip."""
    t0 = time.monotonic()
    closed.spike(400.0, 0.030)
    time.sleep(1.0)
    assert closed.wait_for_event(lambda e: True, timeout=0.0, since=t0) is None
    assert closed.status.state == "CLOSED"


def test_sustained_overcurrent_trips(closed):
    t0 = time.monotonic()
    closed.set_load(350.0)
    ev, _ = _assert_fault_reaction(closed, t0, "PACK_OC", fwconfig.reaction_budget_s(CFG["BMS_DEBOUNCE_OC_SET"]))
    assert ev.value > CFG["BMS_PACK_OC_DISCHARGE_MA"]
    _assert_current_stops(closed)


def test_reset_only_clears_fault_once_condition_is_gone(closed, bms):
    sim = closed
    sim.set_cell_temp(3, 65.0)
    assert sim.wait_for_status(lambda s: s.state == "FAULT", timeout=2)

    # Still hot: reset must be refused.
    t1 = sim.send_command("reset")
    assert sim.wait_for_status(lambda s: s.state == "OPEN", timeout=0.5, since=t1) is None
    assert bms.wait_for_log("reset rejected", timeout=1.0)

    # Below the trip limit but inside the hysteresis band (55-60 degC): still refused.
    sim.set_cell_temp(3, 57.0)
    time.sleep((CFG["BMS_DEBOUNCE_CLEAR"] + 2) * CFG["BMS_MONITOR_PERIOD_MS"] / 1000.0)
    t2 = sim.send_command("reset")
    assert sim.wait_for_status(lambda s: s.state == "OPEN", timeout=0.5, since=t2) is None

    # Properly cooled for long enough: reset accepted, back to OPEN (not CLOSED).
    sim.set_cell_temp(3, 30.0)
    time.sleep((CFG["BMS_DEBOUNCE_CLEAR"] + 2) * CFG["BMS_MONITOR_PERIOD_MS"] / 1000.0)
    t3 = sim.send_command("reset")
    st = sim.wait_for_status(lambda s: s.state == "OPEN", timeout=1.0, since=t3)
    assert st is not None and st.faults_latched == 0
    time.sleep(0.3)
    assert sim.status.state == "OPEN", "BMS must not re-close on its own after a reset"


def test_precharge_timeout_latches_fault(bms, sim):
    assert sim.wait_for_status(lambda s: s.meas_valid, timeout=3)
    sim.set_precharge_fault(True)
    t0 = sim.send_command("close")
    ev = sim.wait_for_event(lambda e: e.fault == "PRECHARGE", timeout=3, since=t0)
    assert ev is not None
    st = sim.wait_for_status(lambda s: s.state == "FAULT", timeout=1, since=t0)
    elapsed = st.t - t0
    timeout_s = CFG["BMS_PRECHARGE_TIMEOUT_MS"] / 1000.0
    _report("precharge timeout", elapsed, timeout_s)
    assert timeout_s - 0.02 <= elapsed <= timeout_s + 0.1 + SLACK_S


def test_measurement_loss_latches_timeout_fault(closed):
    """A silent sensor is not a healthy sensor: the BMS must fail safe."""
    budget = (CFG["BMS_MEAS_TIMEOUT_MS"] + 2 * CFG["BMS_MONITOR_PERIOD_MS"] + fwconfig.PROCESSING_MARGIN_MS) / 1000.0 + SLACK_S
    t0 = time.monotonic()
    closed.set_mute(True)
    _assert_fault_reaction(closed, t0, "MEAS_TIMEOUT", budget)


def test_watchdog_detects_stalled_task(closed, bms):
    budget = (CFG["BMS_WDG_TIMEOUT_MS"] + CFG["BMS_MONITOR_PERIOD_MS"] + CFG["BMS_SUPERVISOR_PERIOD_MS"]
              + fwconfig.PROCESSING_MARGIN_MS) / 1000.0 + SLACK_S
    t0 = closed.stall_task("monitor", 1000)
    ev, _ = _assert_fault_reaction(closed, t0, "WATCHDOG", budget)
    assert ev.value & (1 << p.TASKS["monitor"])
    assert bms.wait_for_log("task 'monitor' stalled", timeout=1.0)

    # Once the task is healthy again the fault can be reset.
    assert bms.wait_for_log("task 'monitor' checking in again", timeout=2.0)
    t1 = closed.send_command("reset")
    assert closed.wait_for_status(lambda s: s.state == "OPEN", timeout=1.0, since=t1)


def test_command_with_bad_crc_is_ignored(bms, sim):
    assert sim.wait_for_status(lambda s: s.meas_valid, timeout=3)
    t0 = sim.send_command("close", corrupt_crc=True)
    assert sim.wait_for_status(lambda s: s.state != "OPEN", timeout=0.5, since=t0) is None
    assert bms.wait_for_log("CRC mismatch", timeout=1.0)
    t1 = sim.send_command("close")  # a good frame still works afterwards
    assert sim.wait_for_status(lambda s: s.state == "CLOSED", timeout=2, since=t1)


def test_debug_command_reports_stacks_and_can_counters(bms, sim):
    """The 'stacks' debug command logs every task's stack high-water mark
    (real numbers on Cortex-M3; a 'not meaningful' note on POSIX) and the CAN
    driver's counters - and receiving it must not disturb normal operation."""
    sim.report_stacks()
    assert bms.wait_for_log("STACK", timeout=2.0)
    assert bms.wait_for_log("CAN    rx", timeout=2.0)
    t = time.monotonic()
    assert sim.wait_for_status(lambda s: s.crc_ok, timeout=1.0, since=t)


def test_soc_tracks_coulomb_count(closed):
    sim = closed
    soc0 = sim.status.soc_pct
    sim.set_load(200.0)
    t0 = time.monotonic()
    time.sleep(3.0)
    sim.set_load(0.0)
    duration = time.monotonic() - t0
    time.sleep(0.3)
    expected_drop = 200.0 * duration / (CFG["BMS_PACK_CAPACITY_MAH"] / 1000.0 * 3600.0) * 100.0
    drop = soc0 - sim.status.soc_pct
    print(f"\nSoC drop {drop:.3f} % (expected ~{expected_drop:.3f} %)")
    assert drop == pytest.approx(expected_drop, rel=0.2)
