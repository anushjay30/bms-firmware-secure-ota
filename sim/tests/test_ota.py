"""Firmware update scenarios: bootloader, A/B slots and the update over CAN.

QEMU only. Every test starts from a factory-reset device (v1.0.0 in slot A,
confirmed; slot B and the metadata erased) with the battery simulator
running, and updates it with the same client tools/flash_over_can.py uses.
Images are made at test time from the build output: signed with the dev key
as v2.0.0 (the update), signed with a stranger's key, re-labelled as an
older version, corrupted in transit - plus the two deliberately broken but
properly signed releases from `make test-images`.
"""
from __future__ import annotations

import os
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import can
import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

import bms_image
import bms_ota
from bms_sim import fwconfig
from bms_sim import protocol as p
from boot_meta import SLOT_A, SLOT_B, SLOT_NONE

TARGET = os.environ.get("BMS_TARGET", "posix")
CAN_IF = os.environ.get("BMS_CAN_IF", "slcan0" if TARGET == "qemu" else "vcan0")
BUILD = p.REPO_ROOT / "build" / "qemu"
DEV_KEY = p.REPO_ROOT / "keys" / "dev_ed25519.pem"
CFG = fwconfig.load()
SLACK_S = fwconfig.slack_ms() / 1000.0

V1 = bms_image.version_from_str("1.0.0")
V2 = bms_image.version_from_str("2.0.0")

pytestmark = [pytest.mark.scenario,
              pytest.mark.skipif(TARGET != "qemu", reason="firmware updates need the bootloader: QEMU only")]


@dataclass
class Images:
    good_v2: bytes      # v2.0.0 for slot B, signed with the dev key
    corrupted: bytes    # the same with one payload bit flipped
    foreign_key: bytes  # v2.0.0 signed with somebody else's key
    downgrade: bytes    # validly signed, but v0.9.0 - below the floor

    @staticmethod
    def variant(name: str) -> bytes:
        path = BUILD / "test-images" / f"app_slotB_{name}.signed.bin"
        if not path.exists():
            pytest.skip(f"{path} missing (run: make TARGET=qemu test-images)")
        return path.read_bytes()


@pytest.fixture(scope="module")
def images() -> Images:
    raw = (BUILD / "app_slotB.bin").read_bytes()
    key = serialization.load_pem_private_key(DEV_KEY.read_bytes(), password=None)
    good = bms_image.sign(raw, key, V2)
    corrupted = bytearray(good)
    corrupted[bms_image.HEADER_SIZE + 0x1234] ^= 0x01  # one bit, somewhere in the code
    return Images(good_v2=good, corrupted=bytes(corrupted),
                  foreign_key=bms_image.sign(raw, Ed25519PrivateKey.generate(), V2),
                  downgrade=bms_image.sign(raw, key, bms_image.version_from_str("0.9.0")))


@pytest.fixture
def ota(device):
    bus = can.Bus(interface="socketcan", channel=CAN_IF,
                  can_filters=[{"can_id": bms_ota.RSP_ID, "can_mask": 0x7FF}])
    # BMS_OTA_MAX_FPS: optional bus-load budget for links without priority
    # queueing (run_qemu.sh normally restores CAN priority on slcan0).
    rate = float(os.environ["BMS_OTA_MAX_FPS"]) if os.environ.get("BMS_OTA_MAX_FPS") else None
    yield bms_ota.OtaClient(bus, reply_timeout=2.0, retries=10, max_frame_rate=rate)
    bus.shutdown()


def _report(result: bms_ota.Result) -> None:
    print(f"\nupdate: {result.size} bytes in {result.seconds:.1f} s ({result.rate / 1024:.1f} KiB/s), "
          f"{result.retransmissions} retransmission(s)")


def _assert_still_v1(device, ota) -> None:
    info = ota.info()
    assert (info.running_slot, info.running_version, info.confirmed) == (SLOT_A, V1, True), str(info)
    meta = device.read_meta()
    assert meta is not None and meta.active_slot == SLOT_A and meta.pending_slot == SLOT_NONE, str(meta)


# ---- 1. the happy path ----------------------------------------------------------

def test_valid_update_boots_the_new_version_which_confirms_itself(device, ota, images):
    before = ota.info()
    assert (before.running_slot, before.running_version, before.confirmed) == (SLOT_A, V1, True)

    _report(ota.update(images.good_v2))
    assert device.wait_for_boots(2, 30), "no reboot into the new image"
    trial = ota.wait_for(lambda i: i.running_version == V2, 30)
    assert trial is not None
    assert (trial.running_slot, trial.boot_reason, trial.confirmed) == (SLOT_B, "trial", False)

    confirmed = ota.wait_for(lambda i: i.confirmed, 30)
    assert confirmed is not None, "v2 never confirmed itself"
    assert device.wait_for_log("confirmed after", 5)
    meta = device.read_meta()
    assert (meta.active_slot, meta.confirmed, meta.pending_slot, meta.min_version) == (SLOT_B, 1, SLOT_NONE, V2)


# ---- 2.-4. images the BMS must refuse --------------------------------------------

def test_corrupted_image_is_rejected_and_the_old_version_keeps_running(device, ota, images):
    with pytest.raises(bms_ota.OtaRefused) as refused:
        ota.update(images.corrupted)
    assert (refused.value.stage, refused.value.status_name) == ("Finish", "CrcError")
    assert device.wait_for_log("update failed: CRC mismatch", 5)
    time.sleep(1.0)
    assert device.boots() == 1, "the BMS must not reboot after a rejected update"
    _assert_still_v1(device, ota)


def test_image_signed_with_another_key_is_rejected(device, ota, images):
    with pytest.raises(bms_ota.OtaRefused) as refused:
        ota.update(images.foreign_key)
    assert (refused.value.stage, refused.value.status_name) == ("Finish", "BadSignature")
    assert device.wait_for_log("update failed: bad signature", 5)
    _assert_still_v1(device, ota)


def test_downgrade_is_rejected(device, ota, images):
    # Announced as what it is: refused before a single data frame is sent.
    with pytest.raises(bms_ota.OtaRefused) as refused:
        ota.update(images.downgrade)
    assert (refused.value.stage, refused.value.status_name) == ("Start", "Downgrade")

    # Announced as a newer version: the request is not signed, the header is.
    with pytest.raises(bms_ota.OtaRefused) as refused:
        ota.update(images.downgrade, announce_version=V2)
    assert (refused.value.stage, refused.value.status_name) == ("Finish", "Downgrade")
    _assert_still_v1(device, ota)


# ---- 5. a release that never confirms itself -------------------------------------

@pytest.mark.parametrize("variant", ["noconfirm", "crash"])
def test_new_image_that_never_confirms_is_rolled_back(device, ota, sim, images, variant):
    image = images.variant(variant)
    _report(ota.update(image))

    for trial in range(1, bms_ota_max_trials() + 1):
        assert device.wait_for_boots(1 + trial, 40), f"trial boot {trial} did not happen"
        if variant == "noconfirm":
            # It runs fine but never confirms: power-cycle it (in a vehicle:
            # the next ignition cycle). The crashing one resets itself.
            time.sleep(0.5)
            sim.reboot()
    assert device.wait_for_log("trial image never confirmed itself: rolling back", 40)
    assert device.wait_for_boots(2 + bms_ota_max_trials(), 40)

    back = ota.wait_for(lambda i: i.running_version == V1, 30)
    assert back is not None
    assert (back.running_slot, back.boot_reason, back.confirmed) == (SLOT_A, "rollback", True)
    meta = device.read_meta()
    assert (meta.active_slot, meta.confirmed, meta.min_version) == (SLOT_A, 1, V1)


def bms_ota_max_trials() -> int:
    import boot_meta
    return boot_meta.MAX_TRIAL_BOOTS


# ---- 6. power loss ---------------------------------------------------------------

def test_power_loss_mid_transfer_keeps_the_old_image(device, ota, sim, images):
    image = images.good_v2
    sim.flash_fault(power_loss=True, after_bytes=len(image) // 3)
    assert device.wait_for_log("flash fault armed: power loss", 5)

    quick = bms_ota.OtaClient(ota.bus, reply_timeout=1.0, retries=3,
                              max_frame_rate=1.0 / ota.frame_gap if ota.frame_gap else None)
    with pytest.raises(bms_ota.OtaTimeout):
        quick.update(image)  # the BMS loses power a third of the way in
    assert device.wait_for_boots(2, 30), "no restart after the power cut"
    assert ota.wait_for(lambda i: True, 20) is not None
    _assert_still_v1(device, ota)

    # Nothing was left in a bad state: the same update now goes through.
    _report(ota.update(image))
    assert ota.wait_for(lambda i: i.running_version == V2 and i.confirmed, 60) is not None


def test_power_loss_while_committing_the_update_keeps_the_old_image(device, ota, sim, images):
    image = images.good_v2
    programmed = (len(image) + 7) // 8 * 8  # the whole image, in 8-byte program units
    sim.flash_fault(power_loss=True, after_bytes=programmed + 16)  # half-way through the metadata record
    assert device.wait_for_log("flash fault armed: power loss", 5)

    with pytest.raises(bms_ota.OtaTimeout):
        ota.update(image, finish_timeout=8.0)  # verified, then power fails while staging it
    assert device.wait_for_boots(2, 30), "no restart after the power cut"
    # The bootloader found the torn copy and used the intact one.
    assert device.wait_for_log("other copy invalid", 10)
    assert ota.wait_for(lambda i: True, 20) is not None
    _assert_still_v1(device, ota)


# ---- 7. safety interlock ------------------------------------------------------------

def test_update_is_refused_while_the_contactors_are_closed(device, ota, sim, images):
    t = sim.send_command("close")
    assert sim.wait_for_status(lambda s: s.state == "CLOSED", timeout=3, since=t), "contactors did not close"

    with pytest.raises(bms_ota.OtaRefused) as refused:
        ota.update(images.good_v2)
    assert (refused.value.stage, refused.value.status_name) == ("Start", "NotSafe")
    assert device.wait_for_log("update lock refused: contactors not open", 3)

    # Opening the contactors is never something the update process does.
    time.sleep(0.5)
    assert sim.status.state == "CLOSED"
    _assert_still_v1(device, ota)


def test_cells_are_monitored_and_close_is_refused_during_a_transfer(device, ota, sim, images):
    outcome: dict = {}
    halfway = threading.Event()

    def progress(done: int, total: int, _retrans: int) -> None:
        if done >= total // 3:
            halfway.set()

    def transfer() -> None:
        try:
            outcome["result"] = ota.update(images.good_v2, progress=progress)
        except bms_ota.OtaError as e:
            outcome["error"] = e

    worker = threading.Thread(target=transfer, daemon=True)
    worker.start()
    assert halfway.wait(120), "transfer made no progress"

    # The vehicle asks to close mid-update: refused, nothing moves.
    t = sim.send_command("close")
    assert device.wait_for_log("close rejected: firmware update in progress", 3)
    assert sim.wait_for_status(lambda s: s.state != "OPEN", timeout=0.5, since=t) is None

    # A cell overheats mid-update: still detected and reported.
    budget = fwconfig.reaction_budget_s(CFG["BMS_DEBOUNCE_OT_SET"])
    t0 = time.monotonic()
    sim.set_cell_temp(3, 65.0)
    event = sim.wait_for_event(lambda e: e.fault == "CELL_OT", timeout=budget + 5.0, since=t0)
    assert event is not None, "no CELL_OT fault event during the transfer"
    status = sim.wait_for_status(lambda s: s.state == "FAULT", timeout=2.0, since=t0)
    assert status is not None
    print(f"\nCELL_OT during the transfer -> FAULT: {(status.t - t0) * 1000:.0f} ms (budget {budget * 1000:.0f} ms)")

    worker.join(180)
    assert not worker.is_alive()
    # With a fault latched the new image must not be activated: a reboot
    # would quietly clear the fault.
    error = outcome.get("error")
    assert isinstance(error, bms_ota.OtaRefused) and error.status_name == "NotSafe", outcome
    assert device.boots() == 1
