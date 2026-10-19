#!/usr/bin/env python3
"""Stamp and sign a BMS application image.

  sign_image.py --key keys/dev_ed25519.pem [--version 1.2.3] app_slotB.bin -o app_slotB.signed.bin

Input is the raw image the build produces (objcopy of the slot: 256-byte
header + payload). The tool checks the header layout, sets the release
version (default: the one the build put in), stamps the CRC-32 of the
payload and appends nothing: the Ed25519 signature goes into the last 64
bytes of the header (see tools/bms_image.py for the exact signed bytes).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bms_image  # noqa: E402


def load_private_key(path: Path) -> Ed25519PrivateKey:
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise SystemExit(f"{path} is not an Ed25519 private key")
    return key


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", type=Path, help="raw image from the build (app_slotX.bin)")
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--key", type=Path, required=True, help="Ed25519 private key (PEM)")
    ap.add_argument("--version", help="release version major.minor.patch (default: keep the build's)")
    args = ap.parse_args(argv)

    key = load_private_key(args.key)
    blob = args.image.read_bytes()
    version = bms_image.version_from_str(args.version) if args.version else None
    try:
        signed = bms_image.sign(blob, key, version)
    except ValueError as e:
        raise SystemExit(f"{args.image}: {e}") from None
    args.output.write_bytes(signed)

    h = bms_image.Header.parse(signed)
    pub = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    print(f"{args.output}: v{h.version_str} for slot at {h.load_address:#010x}, {h.image_size} bytes, "
          f"CRC {h.image_crc32:#010x}, signed with key {bms_image.key_id(pub)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
