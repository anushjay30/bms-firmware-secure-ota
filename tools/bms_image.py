"""BMS application image format: parse, stamp, sign and verify.

Mirrors common/include/bms_image.h - the C header is the reference. Shared by
sign_image.py, flash_over_can.py and the scenario tests.

    header (256 bytes)                                        payload
    +-------------------------------------+-----------------+-------------------
    | magic .. flags, reserved (0x00-0xBF) | signature (64)  | vector table, code ...
    +-------------------------------------+-----------------+-------------------
    signature = Ed25519( SHA-512( header[0x00:0xC0] || payload ) )
"""
from __future__ import annotations

import hashlib
import struct
import zlib
from dataclasses import dataclass

MAGIC = 0x49534D42          # b"BMSI"
HEADER_SIZE = 256
HEADER_VERSION = 2
SIGNED_HEADER = 0xC0        # header bytes covered by the signature
SIG_SIZE = 64
_FIELDS = struct.Struct("<IHHIIIIII")  # magic .. flags (32 bytes)


def version_from_str(text: str) -> int:
    major, minor, patch = (int(x) for x in text.split("."))
    if not (0 <= minor <= 255 and 0 <= patch <= 255 and 0 <= major <= 0xFFFF):
        raise ValueError(f"version {text!r} out of range (major.minor.patch, minor/patch <= 255)")
    return (major << 16) | (minor << 8) | patch


def version_to_str(value: int) -> str:
    return f"{value >> 16}.{(value >> 8) & 0xFF}.{value & 0xFF}"


@dataclass
class Header:
    magic: int
    header_size: int
    header_version: int
    image_version: int
    image_size: int
    image_crc32: int
    load_address: int
    vector_table: int
    flags: int
    signature: bytes

    @classmethod
    def parse(cls, blob: bytes) -> "Header":
        if len(blob) < HEADER_SIZE:
            raise ValueError(f"image too short for a header ({len(blob)} bytes)")
        fields = _FIELDS.unpack_from(blob, 0)
        return cls(*fields, signature=bytes(blob[SIGNED_HEADER:HEADER_SIZE]))

    def pack_into(self, buf: bytearray) -> None:
        _FIELDS.pack_into(buf, 0, self.magic, self.header_size, self.header_version, self.image_version,
                          self.image_size, self.image_crc32, self.load_address, self.vector_table, self.flags)
        buf[SIGNED_HEADER:HEADER_SIZE] = self.signature

    @property
    def version_str(self) -> str:
        return version_to_str(self.image_version)


def check_layout(blob: bytes) -> Header:
    """Structural checks the signer insists on before it signs anything."""
    h = Header.parse(blob)
    if h.magic != MAGIC:
        raise ValueError(f"bad magic {h.magic:#010x}: not a BMS image")
    if h.header_size != HEADER_SIZE or h.header_version != HEADER_VERSION:
        raise ValueError(f"unsupported header (size {h.header_size}, version {h.header_version})")
    if h.vector_table != h.load_address + HEADER_SIZE:
        raise ValueError(f"vector table {h.vector_table:#x} is not load address + 0x100")
    if len(blob) != HEADER_SIZE + h.image_size:
        raise ValueError(f"file is {len(blob)} bytes, header says {HEADER_SIZE} + {h.image_size}")
    return h


def payload(blob: bytes) -> bytes:
    h = Header.parse(blob)
    return bytes(blob[HEADER_SIZE:HEADER_SIZE + h.image_size])


def signed_digest(blob: bytes) -> bytes:
    """The 64-byte message the Ed25519 signature is made over."""
    return hashlib.sha512(bytes(blob[:SIGNED_HEADER]) + payload(blob)).digest()


def stamp(blob: bytes, version: int | None = None) -> bytearray:
    """Set the version (optional) and the payload CRC; clear the signature."""
    h = check_layout(blob)
    out = bytearray(blob)
    if version is not None:
        h.image_version = version
    h.image_crc32 = zlib.crc32(payload(blob)) & 0xFFFFFFFF
    h.signature = bytes(SIG_SIZE)
    h.pack_into(out)
    return out


def sign(blob: bytes, private_key, version: int | None = None) -> bytes:
    """private_key: cryptography Ed25519PrivateKey."""
    out = stamp(blob, version)
    out[SIGNED_HEADER:HEADER_SIZE] = private_key.sign(signed_digest(out))
    return bytes(out)


def verify(blob: bytes, public_key) -> str | None:
    """Return None if the image is intact and signed by public_key, else the reason."""
    from cryptography.exceptions import InvalidSignature

    try:
        h = check_layout(blob)
    except ValueError as e:
        return str(e)
    if zlib.crc32(payload(blob)) & 0xFFFFFFFF != h.image_crc32:
        return "CRC mismatch"
    try:
        public_key.verify(h.signature, signed_digest(blob))
    except InvalidSignature:
        return "bad signature"
    return None


def key_id(public_key_raw: bytes) -> str:
    """Short fingerprint of a public key: first 4 bytes of its SHA-256."""
    return hashlib.sha256(public_key_raw).hexdigest()[:8]
