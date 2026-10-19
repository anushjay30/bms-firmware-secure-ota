"""Offline tests of the host tools (tools/): image signing, metadata
records, and their consistency with the C sources and the DBC. No CAN
interface or firmware needed."""
from __future__ import annotations

import re
import struct
import sys

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from bms_sim import protocol as p

sys.path.insert(0, str(p.REPO_ROOT / "tools"))
import bms_image  # noqa: E402
import bms_ota  # noqa: E402
import boot_meta  # noqa: E402
import keygen  # noqa: E402


def _c_value(relpath: str, name: str) -> int:
    text = (p.REPO_ROOT / relpath).read_text()
    m = re.search(rf"#define\s+{name}\s+\(?(0x[0-9A-Fa-f]+|\d+)u?", text)
    assert m, f"{name} not found in {relpath}"
    return int(m.group(1), 0)


def _fake_image(load: int = 0x00120000, payload_size: int = 300) -> bytes:
    payload = bytes((i * 7 + 1) & 0xFF for i in range(payload_size))
    header = bytearray(bms_image.HEADER_SIZE)
    struct.pack_into("<IHHIIIIII", header, 0, bms_image.MAGIC, bms_image.HEADER_SIZE, bms_image.HEADER_VERSION,
                     0x010000, payload_size, 0, load, load + bms_image.HEADER_SIZE, 0)
    return bytes(header) + payload


@pytest.fixture(scope="module")
def key():
    return Ed25519PrivateKey.generate()


# ---- image format ---------------------------------------------------------------

def test_python_and_c_agree_on_the_image_format():
    assert _c_value("common/include/bms_image.h", "BMS_IMAGE_MAGIC") == bms_image.MAGIC
    assert _c_value("common/include/bms_image.h", "BMS_IMAGE_HEADER_SIZE") == bms_image.HEADER_SIZE
    assert _c_value("common/include/bms_image.h", "BMS_IMAGE_HEADER_VERSION") == bms_image.HEADER_VERSION
    assert _c_value("common/include/bms_image.h", "BMS_IMAGE_SIGNED_HEADER") == bms_image.SIGNED_HEADER


def test_signed_image_verifies_and_carries_version_and_crc(key):
    signed = bms_image.sign(_fake_image(), key, bms_image.version_from_str("2.3.4"))
    h = bms_image.Header.parse(signed)
    assert h.version_str == "2.3.4"
    assert bms_image.verify(signed, key.public_key()) is None


def test_tampering_is_detected(key):
    signed = bytearray(bms_image.sign(_fake_image(), key))
    flipped = bytearray(signed)
    flipped[bms_image.HEADER_SIZE + 10] ^= 1
    assert bms_image.verify(bytes(flipped), key.public_key()) == "CRC mismatch"

    refixed = bms_image.stamp(bytes(flipped))  # fix the CRC, keep the old signature
    refixed[bms_image.SIGNED_HEADER:bms_image.HEADER_SIZE] = signed[bms_image.SIGNED_HEADER:bms_image.HEADER_SIZE]
    assert bms_image.verify(bytes(refixed), key.public_key()) == "bad signature"

    other = Ed25519PrivateKey.generate()
    assert bms_image.verify(bytes(signed), other.public_key()) == "bad signature"


def test_signer_refuses_malformed_images(key):
    with pytest.raises(ValueError, match="magic"):
        bms_image.sign(b"\0" * 400, key)
    with pytest.raises(ValueError, match="header says"):
        bms_image.sign(_fake_image() + b"extra", key)


def test_versions_round_trip():
    assert bms_image.version_to_str(bms_image.version_from_str("1.20.255")) == "1.20.255"
    with pytest.raises(ValueError):
        bms_image.version_from_str("1.256.0")


def test_keygen_exports_the_public_key_as_c(tmp_path):
    keygen.generate(tmp_path / "k.pem", force=False)
    keygen.export_c(tmp_path / "k.pub.pem", tmp_path / "k.c")
    text = (tmp_path / "k.c").read_text()
    assert len(re.findall(r"0x[0-9a-f]{2}", text)) == 32
    assert "g_bms_signing_pubkey" in text and "g_bms_signing_key_id" in text
    with pytest.raises(SystemExit):
        keygen.generate(tmp_path / "k.pem", force=False)  # never overwrite a key


# ---- metadata -------------------------------------------------------------------

def test_python_and_c_agree_on_the_metadata_record():
    assert _c_value("common/include/boot_meta.h", "BOOT_META_MAGIC") == boot_meta.MAGIC
    assert _c_value("common/include/boot_meta.h", "BOOT_META_LAYOUT") == boot_meta.LAYOUT
    assert _c_value("common/include/boot_meta.h", "BOOT_MAX_TRIAL_BOOTS") == boot_meta.MAX_TRIAL_BOOTS
    assert _c_value("common/include/boot_meta.h", "BOOT_SLOT_NONE") == boot_meta.SLOT_NONE


def test_metadata_records_round_trip_and_reject_corruption():
    m = boot_meta.Meta(sequence=9, active_slot=1, pending_slot=0, confirmed=0, boot_attempts=2, min_version=0x020000)
    raw = m.pack()
    assert len(raw) == boot_meta.SIZE
    assert boot_meta.Meta.unpack(raw) == m
    torn = bytearray(raw)
    torn[5] ^= 0x40
    assert boot_meta.Meta.unpack(bytes(torn)) is None
    assert boot_meta.Meta.unpack(b"\xff" * 32) is None


def test_newest_copy_wins_across_sequence_wraparound():
    old = boot_meta.Meta(sequence=0xFFFFFFFF, active_slot=0).pack()
    new = boot_meta.Meta(sequence=0, active_slot=1).pack()
    meta, index = boot_meta.newest(old, new)
    assert index == 1 and meta.active_slot == 1


# ---- update protocol --------------------------------------------------------------

def test_update_protocol_constants_match_dbc_and_c():
    assert p.msg("BMS_OtaRequest").frame_id == bms_ota.REQ_ID
    assert p.msg("BMS_OtaData").frame_id == bms_ota.DATA_ID
    assert p.msg("BMS_OtaResponse").frame_id == bms_ota.RSP_ID
    status = p.msg("BMS_OtaResponse").get_signal_by_name("OtaStatus")
    assert {int(k): str(v) for k, v in status.choices.items()} == bms_ota.STATUS

    c_status = {int(v): name for name, v in re.findall(r"(OTA_(?:OK|ERR_\w+))\s*=\s*(\d+)",
                                                      (p.REPO_ROOT / "app/logic/ota_proto.h").read_text())}
    assert sorted(c_status) == sorted(bms_ota.STATUS)
    assert _c_value("app/logic/ota_proto.h", "OTA_DATA_BYTES") == bms_ota.DATA_BYTES


def test_update_ids_are_lower_priority_than_all_operational_traffic():
    operational = [m.frame_id for m in p.db().messages if not m.name.startswith(("BMS_Ota", "BMS_TestHook"))]
    assert min(bms_ota.REQ_ID, bms_ota.DATA_ID, bms_ota.RSP_ID) > max(operational)


def test_slot_of_reads_the_link_address():
    assert bms_ota.slot_of(_fake_image(load=bms_ota.SLOT_BASES[1])) == 1
    assert bms_ota.slot_of(_fake_image(load=bms_ota.SLOT_BASES[0])) == 0
    with pytest.raises(ValueError):
        bms_ota.slot_of(_fake_image(load=0x00080000))
