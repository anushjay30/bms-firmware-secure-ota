"""Offline consistency checks between the DBC, the C sources and the Python
helpers. No CAN interface or firmware binary needed, so these run anywhere."""
from __future__ import annotations

import re

from bms_sim import fwconfig
from bms_sim import protocol as p


def _c_defines(relpath: str, prefix: str) -> dict[str, int]:
    text = (p.REPO_ROOT / relpath).read_text()
    return {m.group(1): int(m.group(2), 0) for m in re.finditer(rf"#define\s+({prefix}\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?", text)}


def test_dbc_ids_match_c_codec():
    ids = _c_defines("app/logic/bms_msgs.h", "BMS_ID_")
    expected = {
        "BMS_ID_FAULT_EVENT": "BMS_FaultEvent",
        "BMS_ID_VCU_COMMAND": "VCU_Command",
        "BMS_ID_STATUS": "BMS_Status",
        "BMS_ID_PACK_MEAS": "SIM_PackMeas",
        "BMS_ID_CELL_VOLTAGES": "SIM_CellVoltages",
        "BMS_ID_CELL_TEMPS": "SIM_CellTemps",
        "BMS_ID_TEST_HOOK": "BMS_TestHook",
        "BMS_ID_OTA_REQUEST": "BMS_OtaRequest",
        "BMS_ID_OTA_DATA": "BMS_OtaData",
        "BMS_ID_OTA_RESPONSE": "BMS_OtaResponse",
    }
    assert set(ids) == set(expected)
    for c_name, msg_name in expected.items():
        assert p.msg(msg_name).frame_id == ids[c_name], c_name
    assert all(not m.is_extended_frame and m.frame_id <= 0x7FF for m in p.db().messages)


def test_dbc_enums_match_python_tables():
    status = p.msg("BMS_Status")
    assert {int(k): str(v) for k, v in status.get_signal_by_name("State").choices.items()} == p.STATES
    event = p.msg("BMS_FaultEvent")
    assert {int(k): str(v) for k, v in event.get_signal_by_name("FaultId").choices.items()} == p.FAULTS


def test_crc8_sae_j1850_check_value():
    assert p.crc8_sae_j1850(b"123456789") ^ 0xFF == 0x4B


def test_command_e2e_roundtrip_and_corruption():
    can_id, data = p.encode_command("close", counter=3)
    assert can_id == 0x060 and len(data) == 8
    assert p.e2e_valid(can_id, data)
    decoded = p.msg("VCU_Command").decode(data, decode_choices=False)
    assert decoded["Command"] == 1 and decoded["Cmd_AliveCounter"] == 3
    _, bad = p.encode_command("close", counter=3, corrupt_crc=True)
    assert not p.e2e_valid(can_id, bad)


def test_status_frame_from_c_unit_test_decodes():
    # Same bytes as tests/test_bms_msgs.c::test_status_layout_and_e2e (CRC byte aside).
    raw = bytes([0, 0x80 | (2 << 4) | 9, 0x04, 0x02, 7999 & 0xFF, 7999 >> 8, 44400 & 0xFF, 44400 >> 8])
    d = p.msg("BMS_Status").decode(raw, decode_choices=False)
    assert d["State"] == 2 and d["MeasValid"] == 1 and d["Status_AliveCounter"] == 9
    assert d["FaultsLatched"] == 4 and d["FaultsActive"] == 2
    assert abs(d["SoC"] - 79.99) < 1e-9 and d["PackVoltage"] == 44400


def test_config_header_parses_and_ot_budget_is_300ms():
    cfg = fwconfig.load()
    assert cfg["BMS_MONITOR_PERIOD_MS"] == 100
    assert cfg["BMS_CELL_OT_DC"] == 600
    # 50 ms sim period + 2 samples x 100 ms + 50 ms margin (default rates, no slack)
    from bms_sim.simulator import PERIOD_SCALE
    if fwconfig.slack_ms() == 0 and PERIOD_SCALE == 1.0:
        assert abs(fwconfig.reaction_budget_s(cfg["BMS_DEBOUNCE_OT_SET"]) - 0.300) < 1e-9
