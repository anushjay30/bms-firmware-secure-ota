"""Equivalent-circuit model of a 12S lithium-ion pack plus its DC link.

Deliberately simple, but physically shaped so the firmware sees realistic
behaviour:
  * cell voltage = OCV(SoC) - I * R_internal   (sags under load)
  * temperature rises with I^2 * R and relaxes towards ambient
  * current only flows while the BMS reports CLOSED contactors
  * the inverter DC link charges through the precharge resistor (RC curve)
    while the BMS is in PRECHARGE, and is discharged when contactors open
Fault injection overrides individual readings on top of the model.
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass

NUM_CELLS = 12
CAPACITY_AH = 50.0

# Open-circuit voltage of an NMC cell vs state of charge (fraction, volts).
OCV_TABLE = [
    (0.00, 3.00), (0.05, 3.35), (0.10, 3.45), (0.20, 3.55), (0.40, 3.65),
    (0.60, 3.80), (0.80, 3.98), (0.90, 4.07), (1.00, 4.18),
]

PRECHARGE_TAU_S = 0.040      # 20 ohm precharge resistor x 2 mF DC-link capacitor
LINK_DISCHARGE_TAU_S = 0.300  # inverter active discharge when contactors open
THERMAL_MASS_J_PER_K = 900.0  # ~1 kg prismatic cell
COOLING_W_PER_K = 1.0

# Repeating "drive cycle" (seconds, amps): + = discharge, - = regen braking.
DRIVE_PROFILE = [(4.0, 20.0), (2.0, 150.0), (4.0, 60.0), (2.0, -80.0), (3.0, 5.0)]


def ocv(soc: float) -> float:
    soc = min(max(soc, 0.0), 1.0)
    for (s0, v0), (s1, v1) in zip(OCV_TABLE, OCV_TABLE[1:]):
        if soc <= s1:
            return v0 + (v1 - v0) * (soc - s0) / (s1 - s0)
    return OCV_TABLE[-1][1]


@dataclass
class Cell:
    soc: float
    r_ohm: float
    temp_c: float
    volt_override_mv: int | None = None
    temp_override_c: float | None = None


class BatteryModel:
    def __init__(self, initial_soc: float = 0.80, seed: int = 1) -> None:
        rng = random.Random(seed)  # fixed seed: small but repeatable cell spread
        self.cells = [
            Cell(soc=initial_soc + rng.uniform(-0.005, 0.005),
                 r_ohm=0.0012 * rng.uniform(0.95, 1.05),
                 temp_c=25.0 + rng.uniform(-0.5, 0.5))
            for _ in range(NUM_CELLS)
        ]
        self.ambient_c = 25.0
        self.load_a = 0.0              # demanded current when connected
        self.profile_start: float | None = None
        self.spike_a = 0.0
        self.spike_until = 0.0
        self.precharge_fault = False   # shorted link / open precharge resistor
        self.bms_state = "OPEN"        # what the BMS last reported
        self.current_a = 0.0
        self.link_v = 0.0

    # ---- model --------------------------------------------------------------

    def demand_a(self, now: float) -> float:
        demand = self.load_a
        if self.profile_start is not None:
            period = sum(d for d, _ in DRIVE_PROFILE)
            t = (now - self.profile_start) % period
            for duration, amps in DRIVE_PROFILE:
                if t < duration:
                    demand = amps
                    break
                t -= duration
        if now < self.spike_until:
            demand = self.spike_a
        return demand

    def step(self, dt: float, now: float) -> None:
        connected = self.bms_state == "CLOSED"
        self.current_a = self.demand_a(now) if connected else 0.0

        for c in self.cells:
            c.soc = min(max(c.soc - self.current_a * dt / 3600.0 / CAPACITY_AH, 0.0), 1.0)
            heat_w = self.current_a ** 2 * c.r_ohm - COOLING_W_PER_K * (c.temp_c - self.ambient_c)
            c.temp_c += heat_w * dt / THERMAL_MASS_J_PER_K

        pack_v = self.pack_voltage_v()
        if self.bms_state == "PRECHARGE":
            target = 0.5 if self.precharge_fault else pack_v
            self.link_v += (target - self.link_v) * (1.0 - math.exp(-dt / PRECHARGE_TAU_S))
        elif connected:
            self.link_v = pack_v
        else:
            self.link_v *= math.exp(-dt / LINK_DISCHARGE_TAU_S)

    # ---- readings (integers in firmware units) ----------------------------

    def cell_mv(self, i: int) -> int:
        c = self.cells[i]
        if c.volt_override_mv is not None:
            return int(c.volt_override_mv)
        return int(round((ocv(c.soc) - self.current_a * c.r_ohm) * 1000.0))

    def cell_temp_c(self, i: int) -> float:
        c = self.cells[i]
        return c.temp_override_c if c.temp_override_c is not None else c.temp_c

    def pack_voltage_v(self) -> float:
        return sum(self.cell_mv(i) for i in range(NUM_CELLS)) / 1000.0

    def soc(self) -> float:
        return sum(c.soc for c in self.cells) / NUM_CELLS

    def clear_overrides(self) -> None:
        for c in self.cells:
            c.volt_override_mv = None
            c.temp_override_c = None
        self.spike_until = 0.0
        self.precharge_fault = False
