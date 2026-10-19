"""Boot metadata records (mirror of common/include/boot_meta.h).

Used by the tests to inspect - and in fault-injection tests, to craft - the
metadata in META through QEMU's GDB stub.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

MAGIC = 0x4D534D42   # b"BMSM"
LAYOUT = 1
SIZE = 32
SLOT_A, SLOT_B, SLOT_NONE = 0, 1, 0xFF
MAX_TRIAL_BOOTS = 3
_BODY = struct.Struct("<IHHIBBBBIII")  # everything before the CRC (28 bytes)


def slot_name(slot: int) -> str:
    return {SLOT_A: "A", SLOT_B: "B"}.get(slot, "-")


@dataclass
class Meta:
    sequence: int = 1
    active_slot: int = SLOT_A
    pending_slot: int = SLOT_NONE
    confirmed: int = 1
    boot_attempts: int = 0
    min_version: int = 0

    def pack(self) -> bytes:
        body = _BODY.pack(MAGIC, LAYOUT, SIZE, self.sequence, self.active_slot, self.pending_slot,
                          self.confirmed, self.boot_attempts, self.min_version, 0, 0)
        return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)

    @classmethod
    def unpack(cls, raw: bytes) -> "Meta | None":
        """The record, or None if this copy is not valid (erased, torn, corrupted)."""
        if len(raw) < SIZE:
            return None
        body, (crc,) = raw[:_BODY.size], struct.unpack_from("<I", raw, _BODY.size)
        magic, layout, size, seq, active, pending, confirmed, attempts, min_version, _, _ = _BODY.unpack(body)
        if magic != MAGIC or layout != LAYOUT or size != SIZE or crc != zlib.crc32(body) & 0xFFFFFFFF:
            return None
        if active > SLOT_B or pending not in (SLOT_A, SLOT_B, SLOT_NONE) or confirmed > 1:
            return None
        return cls(seq, active, pending, confirmed, attempts, min_version)

    def __str__(self) -> str:
        state = "confirmed" if self.confirmed else f"on trial ({self.boot_attempts} boots)"
        v = self.min_version
        return (f"seq {self.sequence}: active {slot_name(self.active_slot)} {state}, "
                f"pending {slot_name(self.pending_slot)}, min v{v >> 16}.{(v >> 8) & 0xFF}.{v & 0xFF}")


def newest(copy0: bytes, copy1: bytes) -> tuple["Meta | None", int]:
    """Same selection rule as boot_meta_load(): highest valid sequence (wrap-safe)."""
    a, b = Meta.unpack(copy0), Meta.unpack(copy1)
    if a and b:
        return (b, 1) if 0 < ((b.sequence - a.sequence) & 0xFFFFFFFF) < 0x80000000 else (a, 0)
    if a:
        return a, 0
    if b:
        return b, 1
    return None, -1
