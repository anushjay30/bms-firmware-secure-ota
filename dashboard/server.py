"""BMS dashboard: a FastAPI + WebSocket backend that broadcasts BMS state to a
single-page frontend (dashboard/frontend/).

Two ways to feed it:

* In-process (the demo video): the orchestrator owns the one CAN connection,
  runs the battery simulator and the OTA client on it, and calls hub.publish()
  directly. Needed on Windows, where QEMU's CAN UART over TCP accepts a single
  client, so the dashboard cannot open its own second connection.

* Standalone live (Linux/SocketCAN): `python -m dashboard --channel slcan0`
  opens its own read-only CAN connection and feeds the hub, for watching a
  running BMS. (On SocketCAN many readers can share one bus.)

The hub keeps one JSON snapshot, bumped by publish(); each WebSocket client
loops sending the snapshot whenever its version changes - simple and
thread-safe, no cross-thread asyncio scheduling from the CAN threads.
"""
from __future__ import annotations

import asyncio
import copy
import threading
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

FRONTEND = Path(__file__).resolve().parent / "frontend"


class Hub:
    """The latest dashboard state, shared between the CAN/scenario threads and
    the WebSocket clients."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._version = 0
        self._state: dict = {
            "t": 0.0,
            "cells_mv": [0] * 12,
            "cells_temp": [0.0] * 12,
            "soc_pct": 0.0,
            "pack_mv": 0,
            "pack_current_a": 0.0,
            "state": "OPEN",
            "meas_valid": False,
            "faults_latched": 0,
            "faults_active": 0,
            "fault_log": [],       # [{t, text}]
            "ota": {"active_slot": "A", "version": "1.0.0", "status": "confirmed",
                    "pending": None, "progress": None, "note": ""},
            "uart": [],            # recent firmware log lines
            "caption": {"text": "", "kind": "info"},
            "limits": {"ov_mv": 4250, "uv_mv": 2800, "ot_dc": 600},
        }

    def publish(self, **partial) -> None:
        with self._lock:
            for key, value in partial.items():
                self._state[key] = value
            self._version += 1

    def append(self, key: str, value, cap: int) -> None:
        with self._lock:
            lst = list(self._state.get(key, []))
            lst.append(value)
            self._state[key] = lst[-cap:]
            self._version += 1

    def snapshot(self) -> tuple[int, dict]:
        with self._lock:
            return self._version, copy.deepcopy(self._state)


hub = Hub()


def create_app() -> FastAPI:
    app = FastAPI(title="BMS dashboard")

    @app.get("/")
    async def index() -> FileResponse:
        return FileResponse(FRONTEND / "index.html")

    app.mount("/static", StaticFiles(directory=FRONTEND), name="static")

    @app.websocket("/ws")
    async def ws(socket: WebSocket) -> None:
        await socket.accept()
        last_sent = -1
        try:
            while True:
                version, state = hub.snapshot()
                if version != last_sent:
                    await socket.send_json(state)
                    last_sent = version
                await asyncio.sleep(0.02)  # ~50 Hz poll; sends only on change
        except (WebSocketDisconnect, RuntimeError):
            return

    return app


app = create_app()


def serve_in_thread(host: str = "127.0.0.1", port: int = 8000) -> threading.Thread:
    """Run uvicorn in a daemon thread; returns once it is accepting connections."""
    import uvicorn

    config = uvicorn.Config(app, host=host, port=port, log_level="warning")
    server = uvicorn.Server(config)
    thread = threading.Thread(target=server.run, name="dashboard", daemon=True)
    thread.start()

    import socket as _socket
    import time
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        with _socket.socket() as s:
            if s.connect_ex((host, port)) == 0:
                return thread
        time.sleep(0.05)
    raise RuntimeError("dashboard server did not start")


def _standalone() -> int:
    """Live, read-only dashboard against a running BMS (Linux/SocketCAN)."""
    import argparse
    import sys
    import time

    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sim"))
    from bms_sim import protocol as p

    ap = argparse.ArgumentParser(description="Live BMS dashboard (read-only).")
    ap.add_argument("--channel", default="slcan0")
    ap.add_argument("--interface", default="socketcan")
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()

    import can

    serve_in_thread(port=args.port)
    print(f"dashboard on http://127.0.0.1:{args.port}")
    bus = can.Bus(interface=args.interface, channel=args.channel)
    status_id = p.msg("BMS_Status").frame_id
    event_id = p.msg("BMS_FaultEvent").frame_id
    volt_id = p.msg("SIM_CellVoltages").frame_id
    temp_id = p.msg("SIM_CellTemps").frame_id
    cells_mv = [0] * 12
    cells_temp = [0.0] * 12
    t0 = time.monotonic()
    while True:
        m = bus.recv(timeout=1.0)
        if m is None:
            continue
        try:
            if m.arbitration_id == status_id:
                d = p.msg("BMS_Status").decode(m.data, decode_choices=False)
                hub.publish(t=time.monotonic() - t0, state=p.STATES.get(d["State"], "?"),
                            soc_pct=float(d["SoC"]), pack_mv=int(d["PackVoltage"]),
                            meas_valid=bool(d["MeasValid"]), faults_latched=int(d["FaultsLatched"]),
                            faults_active=int(d["FaultsActive"]))
            elif m.arbitration_id == volt_id:
                d = p.msg("SIM_CellVoltages").decode(m.data, decode_choices=False)
                page = int(d["CellVoltPage"])
                for i in range(3):
                    cells_mv[page * 3 + i] = int(d[f"CellVoltage_{page*3+i+1:02d}"])
                hub.publish(cells_mv=list(cells_mv))
            elif m.arbitration_id == temp_id:
                d = p.msg("SIM_CellTemps").decode(m.data, decode_choices=False)
                page = int(d["CellTempPage"])
                for i in range(3):
                    cells_temp[page * 3 + i] = float(d[f"CellTemp_{page*3+i+1:02d}"])
                hub.publish(cells_temp=list(cells_temp))
            elif m.arbitration_id == event_id:
                d = p.msg("BMS_FaultEvent").decode(m.data, decode_choices=False)
                hub.append("fault_log", {"t": f"{time.monotonic()-t0:05.1f}",
                           "text": f"{p.FAULTS.get(d['FaultId'], d['FaultId'])} cell {d['CellNumber']}"}, 8)
        except (KeyError, ValueError):
            continue


if __name__ == "__main__":
    raise SystemExit(_standalone())
