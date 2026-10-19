"""Interactive battery simulator: `python -m bms_sim [--channel vcan0]`.

Type `help` at the prompt. State changes and fault events from the BMS are
printed as they arrive.
"""
from __future__ import annotations

import argparse
import cmd
import sys
import time

from . import protocol as p
from .simulator import FaultEvent, Simulator, Status

FAULT_UNITS = {"CELL_OV": "mV", "CELL_UV": "mV", "CELL_OT": "x0.1 degC", "PACK_OC": "mA",
               "MEAS_TIMEOUT": "ms", "PRECHARGE": "mV link", "WATCHDOG": "task mask"}


def _print_async(text: str) -> None:
    # Print above the prompt without mangling what the user is typing.
    sys.stdout.write(f"\r{text}\nbms> ")
    sys.stdout.flush()


def _on_status(s: Status, prev: Status | None) -> None:
    before = prev.state if prev else "?"
    faults = ",".join(p.fault_bits(s.faults_latched)) or "none"
    _print_async(f"[BMS] state {before} -> {s.state}  SoC {s.soc_pct:.2f}%  meas_valid={int(s.meas_valid)}  latched={faults}")


def _on_event(e: FaultEvent) -> None:
    cell = f" cell {e.cell}" if e.cell else ""
    _print_async(f"[BMS] FAULT {e.fault}{cell} value {e.value} {FAULT_UNITS.get(e.fault, '')}")


def _cell(arg: str) -> int:
    cell = int(arg)
    if not 1 <= cell <= p.NUM_CELLS:
        raise ValueError(f"cell must be 1..{p.NUM_CELLS}")
    return cell


class Shell(cmd.Cmd):
    intro = "BMS battery simulator. Type 'help' for commands, 'quit' to exit."
    prompt = "bms> "

    def __init__(self, sim: Simulator, interface: str = "socketcan", channel: str = "vcan0") -> None:
        super().__init__()
        self.sim = sim
        self.interface = interface
        self.channel = channel

    # Errors in a command should not kill the shell.
    def onecmd(self, line: str) -> bool:
        try:
            return super().onecmd(line)
        except (ValueError, IndexError, KeyError) as exc:
            print(f"error: {exc}")
            return False

    def emptyline(self) -> bool:
        return False

    # ---- VCU commands ----
    def do_close(self, _: str) -> None:
        """close: ask the BMS to precharge and close the contactors"""
        self.sim.send_command("close")

    def do_open(self, _: str) -> None:
        """open: ask the BMS to open the contactors"""
        self.sim.send_command("open")

    def do_reset(self, _: str) -> None:
        """reset: request a fault reset (only succeeds once conditions are gone)"""
        self.sim.send_command("reset")

    def do_badcrc(self, _: str) -> None:
        """badcrc: send a CLOSE command with a corrupted E2E CRC (should be ignored)"""
        self.sim.send_command("close", corrupt_crc=True)

    def do_status(self, _: str) -> None:
        """status: show the latest BMS status and the simulated pack"""
        s = self.sim.status
        m = self.sim.model
        if s is None:
            print("no BMS_Status received yet - is the firmware running?")
        else:
            age = time.monotonic() - s.t
            print(f"BMS : {s.state}  SoC {s.soc_pct:.2f}%  pack {s.pack_mv} mV  meas_valid={int(s.meas_valid)}  "
                  f"latched={p.fault_bits(s.faults_latched)} active={p.fault_bits(s.faults_active)}  "
                  f"crc_ok={s.crc_ok}  ({age * 1000:.0f} ms ago)")
        print(f"SIM : I={m.current_a:.1f} A  link={m.link_v:.2f} V  pack={m.pack_voltage_v():.2f} V  "
              f"SoC={m.soc() * 100:.2f}%  tx_dropped={self.sim.tx_dropped}")
        print("cells mV : " + " ".join(f"{m.cell_mv(i):5d}" for i in range(p.NUM_CELLS)))
        print("cells C  : " + " ".join(f"{m.cell_temp_c(i):5.1f}" for i in range(p.NUM_CELLS)))

    # ---- fault injection ----
    def do_heat(self, arg: str) -> None:
        """heat <cell> <degC>: force a cell temperature, e.g. 'heat 5 65'"""
        cell, degc = arg.split()
        self.sim.set_cell_temp(_cell(cell), float(degc))

    def do_cool(self, arg: str) -> None:
        """cool <cell>: return a cell's temperature to the model"""
        self.sim.set_cell_temp(_cell(arg), None)

    def do_cellv(self, arg: str) -> None:
        """cellv <cell> <mV>: force a cell voltage, e.g. 'cellv 2 2500'"""
        cell, mv = arg.split()
        self.sim.set_cell_voltage(_cell(cell), int(mv))

    def do_clearv(self, arg: str) -> None:
        """clearv <cell>: return a cell's voltage to the model"""
        self.sim.set_cell_voltage(_cell(arg), None)

    def do_load(self, arg: str) -> None:
        """load <amps>: set the load current (+ discharge, - charge); flows only when CLOSED"""
        self.sim.set_load(float(arg))

    def do_drive(self, arg: str) -> None:
        """drive on|off: run a repeating drive-cycle current profile"""
        self.sim.drive_profile(arg.strip() != "off")

    def do_spike(self, arg: str) -> None:
        """spike <amps> <ms>: overcurrent spike, e.g. 'spike 400 500'"""
        amps, ms = arg.split()
        self.sim.spike(float(amps), int(ms) / 1000.0)

    def do_prechargefail(self, arg: str) -> None:
        """prechargefail on|off: DC link refuses to charge (shorted link / open resistor)"""
        self.sim.set_precharge_fault(arg.strip() != "off")

    def do_mute(self, arg: str) -> None:
        """mute on|off: stop sending all measurement frames (sensor loss)"""
        self.sim.set_mute(arg.strip() != "off")

    def do_stall(self, arg: str) -> None:
        """stall <task> <ms>: test hook, stop a firmware task checking in, e.g. 'stall monitor 1000'"""
        task, ms = arg.split()
        self.sim.stall_task(task, int(ms))

    def do_stacks(self, _: str) -> None:
        """stacks: ask the firmware to log every task's stack high-water mark"""
        self.sim.report_stacks()

    def do_reboot(self, _: str) -> None:
        """reboot: test hook, full system reset of the BMS"""
        self.sim.reboot()

    def do_crash(self, _: str) -> None:
        """crash: test hook, make the firmware fault (Cortex-M: HardFault register dump, then reset)"""
        self.sim.trigger_fault()

    def do_update(self, arg: str) -> None:
        """update [image]: flash a signed image over CAN (QEMU only); default: the inactive slot's
        build/qemu/app_slot?.signed.bin. Shows progress; needs contactors OPEN."""
        import sys
        from pathlib import Path
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
        import flash_over_can  # noqa: E402
        args = ["--interface", self.interface, "--channel", self.channel]
        if arg.strip():
            args += ["--image", arg.strip()]
        flash_over_can.main(args)

    def do_otainfo(self, _: str) -> None:
        """otainfo: ask the firmware which slot/version it runs and its update state (QEMU only)"""
        import sys
        from pathlib import Path
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
        import flash_over_can  # noqa: E402
        flash_over_can.main(["--interface", self.interface, "--channel", self.channel, "--info"])

    def do_clear(self, _: str) -> None:
        """clear: remove every injected fault"""
        self.sim.clear_overrides()

    def do_quit(self, _: str) -> bool:
        """quit: exit"""
        return True

    do_exit = do_quit
    do_EOF = do_quit


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--channel", default="vcan0")
    ap.add_argument("--interface", default="socketcan", help="python-can interface (default socketcan)")
    ap.add_argument("--load", type=float, default=20.0, help="initial load current in A")
    args = ap.parse_args(argv)

    with Simulator(args.channel, args.interface, on_status_change=_on_status, on_fault_event=_on_event) as sim:
        sim.set_load(args.load)
        Shell(sim, args.interface, args.channel).cmdloop()
    return 0
