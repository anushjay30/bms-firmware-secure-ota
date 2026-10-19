"""CAN protocol helpers shared by the simulator, CLI and tests.

Everything about message layout comes from dbc/bms.dbc via cantools; this
module only adds what a DBC cannot express: the E2E CRC/counter scheme and
friendly names for enums and bitfields.
"""
from __future__ import annotations

import functools
from pathlib import Path

import cantools

REPO_ROOT = Path(__file__).resolve().parents[2]
DBC_PATH = REPO_ROOT / "dbc" / "bms.dbc"

NUM_CELLS = 12
CELLS_PER_PAGE = 3
PAGES = NUM_CELLS // CELLS_PER_PAGE

STATES = {0: "OPEN", 1: "PRECHARGE", 2: "CLOSED", 3: "FAULT"}
STATE_IDS = {v: k for k, v in STATES.items()}

FAULTS = {0: "CELL_OV", 1: "CELL_UV", 2: "CELL_OT", 3: "PACK_OC", 4: "MEAS_TIMEOUT", 5: "PRECHARGE", 6: "WATCHDOG"}
FAULT_IDS = {v: k for k, v in FAULTS.items()}

COMMANDS = {"none": 0, "close": 1, "open": 2, "reset": 3}
TASKS = {"can_rx": 0, "monitor": 1, "fault": 2, "contactor": 3, "can_tx": 4, "logger": 5, "ota": 6}
HOOKS = {"none": 0, "stall": 1, "stacks": 2, "reboot": 3, "fault": 4, "flash_fail_write": 5, "flash_power_loss": 6}


@functools.lru_cache(maxsize=1)
def db() -> cantools.database.can.Database:
    return cantools.database.load_file(str(DBC_PATH), strict=True)


def msg(name: str):
    return db().get_message_by_name(name)


def fault_bits(mask: int) -> list[str]:
    return [name for bit, name in FAULTS.items() if mask & (1 << bit)]


# ---- E2E (must match common/src/e2e.c and crc8.c) -------------------------

def crc8_sae_j1850(data: bytes, crc: int = 0xFF) -> int:
    """Running CRC-8 SAE J1850 (poly 0x1D), without the final XOR."""
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1D) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def e2e_crc(can_id: int, payload: bytes) -> int:
    data_id = bytes([can_id & 0xFF, (can_id >> 8) & 0xFF])
    return crc8_sae_j1850(data_id + payload[1:]) ^ 0xFF


def e2e_valid(can_id: int, payload: bytes) -> bool:
    return len(payload) >= 2 and payload[0] == e2e_crc(can_id, payload)


def encode_command(command: str | int, counter: int, corrupt_crc: bool = False) -> tuple[int, bytes]:
    """Build an E2E-protected VCU_Command frame. Returns (can_id, payload)."""
    cmd = COMMANDS[command] if isinstance(command, str) else int(command)
    m = msg("VCU_Command")
    payload = bytearray(m.encode({"Cmd_CRC": 0, "Cmd_AliveCounter": counter & 0xF, "Command": cmd}))
    payload[0] = e2e_crc(m.frame_id, bytes(payload))
    if corrupt_crc:
        payload[0] ^= 0x5A
    return m.frame_id, bytes(payload)
