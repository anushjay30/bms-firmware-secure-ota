"""Firmware update over CAN: the tester side of the BMS update protocol.

Protocol (app/logic/ota_proto.h, dbc/bms.dbc, README "Update protocol"):

    0x6C0 BMS_OtaRequest   Start(slot, size, version) | Finish | Abort | GetInfo
    0x6C1 BMS_OtaData      [seq = frame index mod 256][7 image bytes]
    0x6C8 BMS_OtaResponse  [type][status][...]: replies, and one DataAck per
                           window (status Sequence = resend from NextFrame)

The tester keeps at most one window in flight (flow control), resends from
the last acknowledged frame when an ACK does not come (retransmission), and
gives up after a number of silent retries (timeout).
"""
from __future__ import annotations

import queue
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

import can

import bms_image

REQ_ID, DATA_ID, RSP_ID = 0x6C0, 0x6C1, 0x6C8
REQ_START, REQ_FINISH, REQ_ABORT, REQ_INFO = 1, 2, 3, 4
RSP_START, RSP_FINISH, RSP_ABORT, RSP_INFO, RSP_DATA_ACK = 1, 2, 3, 4, 5
DATA_BYTES = 7

# Must match ota_status_t (app/logic/ota_proto.h) and the DBC's VAL_ table.
STATUS = {0: "Ok", 1: "NotSafe", 2: "Busy", 3: "BadSlot", 4: "TooLarge", 5: "Downgrade", 6: "Sequence",
          7: "CrcError", 8: "BadSignature", 9: "BadImage", 10: "FlashError", 11: "NoSession", 12: "Timeout",
          13: "Incomplete"}
BOOT_REASONS = {0: "normal", 1: "trial", 2: "rollback", 3: "fallback", 4: "none"}


def _slot_bases() -> dict[int, int]:
    """Slot addresses from the same header the firmware is built with."""
    header = Path(__file__).resolve().parents[1] / "port" / "qemu-mps2" / "memory_map.h"
    values = {}
    for line in header.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "#define" and parts[1] in ("MM_SLOT_A_BASE", "MM_SLOT_B_BASE"):
            values[parts[1]] = int(parts[2], 0)
    return {0: values["MM_SLOT_A_BASE"], 1: values["MM_SLOT_B_BASE"]}


SLOT_BASES = _slot_bases()


def slot_of(image: bytes) -> int:
    """Which slot an image was linked for (it runs in place, so only there)."""
    load = bms_image.Header.parse(image).load_address
    for slot, base in SLOT_BASES.items():
        if base == load:
            return slot
    raise ValueError(f"image linked for {load:#010x}, which is neither slot")


class OtaError(Exception):
    pass


class OtaRefused(OtaError):
    """The BMS answered, and said no."""

    def __init__(self, stage: str, status: int) -> None:
        self.stage, self.status = stage, status
        super().__init__(f"{stage} refused by the BMS: {STATUS.get(status, status)}")

    @property
    def status_name(self) -> str:
        return STATUS.get(self.status, str(self.status))


class OtaTimeout(OtaError):
    """The BMS stopped answering."""


@dataclass
class Info:
    running_slot: int
    confirmed: bool
    session_active: bool
    update_allowed: bool
    running_version: int
    boot_reason: str
    boot_attempt: int

    @property
    def inactive_slot(self) -> int:
        return 1 - self.running_slot

    def __str__(self) -> str:
        state = "confirmed" if self.confirmed else f"on trial (boot {self.boot_attempt})"
        return (f"v{bms_image.version_to_str(self.running_version)} in slot {'AB'[self.running_slot]}, {state}, "
                f"booted: {self.boot_reason}, update {'allowed' if self.update_allowed else 'not allowed now'}")


@dataclass
class Result:
    version: int
    slot: int
    size: int
    seconds: float
    retransmissions: int

    @property
    def rate(self) -> float:
        return self.size / self.seconds if self.seconds > 0 else 0.0


ProgressFn = Callable[[int, int, int], None]  # (bytes acknowledged, total bytes, retransmissions)


class OtaClient:
    def __init__(self, bus: can.BusABC, reply_timeout: float = 1.0, retries: int = 8,
                 max_frame_rate: Optional[float] = None, inbox: "queue.Queue | None" = None) -> None:
        """max_frame_rate: pace data frames to at most this many per second -
        a bus-load budget, as production flash tools keep, so an update leaves
        room for operational traffic on a link without CAN arbitration.

        inbox: if given, response frames are read from this queue of
        can.Message instead of bus.recv() - for sharing one bus connection
        with another reader (the demo dashboard feeds it from its RX loop)."""
        self.bus = bus
        self.reply_timeout = reply_timeout
        self.retries = retries
        self.frame_gap = 1.0 / max_frame_rate if max_frame_rate else 0.0
        self._next_frame_t = 0.0
        self.inbox = inbox

    # ---- frames ------------------------------------------------------------

    def _send(self, can_id: int, data: bytes, timeout: float = 1.0) -> None:
        """A full transmit queue (ENOBUFS on SocketCAN) is back-pressure on a
        slow link like slcan: wait and retry instead of failing."""
        msg = can.Message(arbitration_id=can_id, data=data, is_extended_id=False)
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.bus.send(msg, timeout=0.05)
                return
            except can.CanOperationError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.002)

    def _pace(self) -> None:
        if self.frame_gap <= 0.0:
            return
        now = time.monotonic()
        if now < self._next_frame_t:
            time.sleep(self._next_frame_t - now)
        self._next_frame_t = max(now, self._next_frame_t) + self.frame_gap

    def _request(self, req: int, payload: bytes = b"") -> None:
        self._send(REQ_ID, bytes([req]) + payload.ljust(7, b"\0"))

    def _receive(self, want: set[int], timeout: float) -> Optional[bytes]:
        """Next response of one of the wanted types, or None on timeout."""
        deadline = time.monotonic() + timeout
        while (left := deadline - time.monotonic()) > 0:
            if self.inbox is not None:
                try:
                    m = self.inbox.get(timeout=left)
                except queue.Empty:
                    return None
            else:
                m = self.bus.recv(timeout=left)
            if m is not None and m.arbitration_id == RSP_ID and len(m.data) >= 2 and m.data[0] in want:
                return bytes(m.data)
        return None

    def _drain(self) -> None:
        if self.inbox is not None:
            try:
                while True:
                    self.inbox.get_nowait()
            except queue.Empty:
                return
        else:
            while self.bus.recv(timeout=0) is not None:
                pass

    # ---- requests ----------------------------------------------------------

    def info(self, timeout: Optional[float] = None) -> Info:
        for _ in range(3):
            self._request(REQ_INFO)
            r = self._receive({RSP_INFO}, timeout or self.reply_timeout)
            if r is not None:
                return Info(running_slot=r[2], confirmed=bool(r[3] & 1), session_active=bool(r[3] & 2),
                            update_allowed=bool(r[3] & 4), running_version=r[4] | r[5] << 8 | r[6] << 16,
                            boot_reason=BOOT_REASONS.get(r[7] & 0x0F, "?"), boot_attempt=r[7] >> 4)
        raise OtaTimeout("no reply to GetInfo")

    def abort(self) -> None:
        self._request(REQ_ABORT)
        self._receive({RSP_ABORT}, self.reply_timeout)

    def update(self, image: bytes, progress: Optional[ProgressFn] = None, start_timeout: float = 10.0,
               finish_timeout: float = 20.0, announce_version: Optional[int] = None) -> Result:
        """Send `image` (signed, linked for the BMS's inactive slot) and have
        it staged. On success the BMS reboots into it right away.

        announce_version (tests only) puts a different version in the Start
        request than the image header carries: the BMS must judge the signed
        header, not the unsigned request."""
        header = bms_image.check_layout(image)
        slot = slot_of(image)
        size = len(image)
        total_frames = (size + DATA_BYTES - 1) // DATA_BYTES
        self._drain()

        version = (header.image_version if announce_version is None else announce_version) & 0xFFFFFF
        self._request(REQ_START, bytes([slot]) + size.to_bytes(3, "little") + version.to_bytes(3, "little"))
        r = self._receive({RSP_START}, start_timeout)  # the BMS erases first
        if r is None:
            raise OtaTimeout("no reply to Start")
        if r[1] != 0:
            raise OtaRefused("Start", r[1])
        window = r[2]

        t0 = time.monotonic()
        nxt, retrans, silent = 0, 0, 0
        while nxt < total_frames:
            end = min((nxt // window + 1) * window, total_frames)  # up to the next ACK point
            for i in range(nxt, end):
                chunk = image[i * DATA_BYTES:(i + 1) * DATA_BYTES]
                self._pace()
                self._send(DATA_ID, bytes([i & 0xFF]) + chunk)
            while True:
                r = self._receive({RSP_DATA_ACK}, self.reply_timeout)
                if r is None:
                    silent += 1
                    if silent > self.retries:
                        raise OtaTimeout(f"no acknowledgement after {self.retries} retries (frame {nxt})")
                    retrans += 1  # resend everything since the last ACK
                    break
                ack = r[2] | r[3] << 8 | r[4] << 16
                if r[1] == 0 and ack > nxt:
                    nxt, silent = ack, 0
                    break
                if r[1] == 6 and ack >= nxt:  # Sequence: resend from where the BMS is
                    nxt, retrans = ack, retrans + 1
                    break
                if r[1] not in (0, 6):
                    raise OtaRefused("transfer", r[1])
                # anything else is a stale reply to an earlier window: keep waiting
            if progress:
                progress(min(nxt * DATA_BYTES, size), size, retrans)

        self._request(REQ_FINISH)  # the BMS verifies CRC, signature and version now
        r = self._receive({RSP_FINISH}, finish_timeout)
        if r is None:
            raise OtaTimeout("no reply to Finish")
        if r[1] != 0:
            raise OtaRefused("Finish", r[1])
        return Result(version=header.image_version, slot=slot, size=size, seconds=time.monotonic() - t0,
                      retransmissions=retrans)

    def wait_for(self, predicate: Callable[[Info], bool], timeout: float) -> Optional[Info]:
        """Poll GetInfo until predicate(info) holds (e.g. after a reboot)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                info = self.info(timeout=0.5)
                if predicate(info):
                    return info
            except OtaTimeout:
                pass  # rebooting
            time.sleep(0.2)
        return None
