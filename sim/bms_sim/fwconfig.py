"""Firmware calibration values, read from app/include/bms_config.h.

The scenario tests derive their timing budgets from the same numbers the
firmware is compiled with, so changing a debounce count in C automatically
changes what the tests expect.
"""
from __future__ import annotations

import functools
import os
import re

from .protocol import REPO_ROOT
from .simulator import CELL_PERIOD_S

CONFIG_H = REPO_ROOT / "app" / "include" / "bms_config.h"

# Time for the firmware to process a sample, update state and get the frame
# onto the bus, plus Python receive overhead.
PROCESSING_MARGIN_MS = 50


@functools.lru_cache(maxsize=1)
def load() -> dict[str, int]:
    text = CONFIG_H.read_text()
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"^#define\s+(BMS_\w+)\s+(-?\d+)", text, re.M)}


def slack_ms() -> int:
    """Extra allowance for slow or virtualised hosts (BMS_TIMING_SLACK_MS)."""
    return int(os.environ.get("BMS_TIMING_SLACK_MS", "0"))


def reaction_budget_s(debounce_set: int) -> float:
    """Worst case from injecting a bad value until the BMS reports FAULT.

    sim send period          until the new value is on the bus
    + N monitor periods      the first sample may be a whole period away,
                             then N-1 more consecutive samples to debounce
    + processing margin      fault -> contactor -> CAN TX
    """
    cfg = load()
    ms = CELL_PERIOD_S * 1000 + debounce_set * cfg["BMS_MONITOR_PERIOD_MS"] + PROCESSING_MARGIN_MS + slack_ms()
    return ms / 1000.0
