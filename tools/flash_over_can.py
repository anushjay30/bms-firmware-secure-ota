#!/usr/bin/env python3
"""Update the BMS firmware over CAN.

  flash_over_can.py [--channel slcan0]                 build/qemu/app_slot{A,B}.signed.bin
  flash_over_can.py --slot-a A.bin --slot-b B.bin      explicit images
  flash_over_can.py --image B.bin                      one image (must match the inactive slot)
  flash_over_can.py --info                             only show what the BMS runs

The application executes in place, so an image only runs from the slot it
was linked for. The tool asks the BMS which slot it runs from and sends the
image linked for the other one. Any python-can interface works: SocketCAN
(vcan0, slcan0) by default, or e.g. --interface slcan --channel
socket://127.0.0.1:5555 for QEMU's UART on TCP.

Exit status: 0 updated, 1 refused by the BMS, 2 transfer failed (no answer),
3 bad arguments or images.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import can

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bms_image  # noqa: E402
import bms_ota  # noqa: E402

REPO = Path(__file__).resolve().parents[1]


def progress_bar(done: int, total: int, retrans: int, t0: float, width: int = 30) -> None:
    frac = done / total if total else 1.0
    rate = done / max(time.monotonic() - t0, 1e-6) / 1024
    bar = "#" * int(frac * width)
    sys.stderr.write(f"\r  [{bar:<{width}}] {frac * 100:5.1f}%  {done}/{total} bytes  {rate:5.1f} KiB/s"
                     f"  retransmissions {retrans}")
    sys.stderr.flush()


def pick_image(args, info: bms_ota.Info) -> bytes:
    if args.image:
        image = args.image.read_bytes()
        if bms_ota.slot_of(image) != info.inactive_slot:
            raise SystemExit(f"{args.image} is linked for slot {'AB'[bms_ota.slot_of(image)]}, but the BMS runs "
                             f"from slot {'AB'[info.running_slot]}: it needs the image for slot "
                             f"{'AB'[info.inactive_slot]}")
        return image
    paths = {0: args.slot_a or args.build_dir / "app_slotA.signed.bin",
             1: args.slot_b or args.build_dir / "app_slotB.signed.bin"}
    path = paths[info.inactive_slot]
    image = path.read_bytes()
    if bms_ota.slot_of(image) != info.inactive_slot:
        raise SystemExit(f"{path} is not linked for slot {'AB'[info.inactive_slot]}")
    return image


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--interface", default="socketcan", help="python-can interface (default socketcan)")
    ap.add_argument("--channel", default="slcan0", help="CAN channel (default slcan0)")
    ap.add_argument("--bitrate", type=int, default=500000)
    ap.add_argument("--image", type=Path, help="a single signed image")
    ap.add_argument("--slot-a", type=Path, help="signed image linked for slot A")
    ap.add_argument("--slot-b", type=Path, help="signed image linked for slot B")
    ap.add_argument("--build-dir", type=Path, default=REPO / "build" / "qemu")
    ap.add_argument("--info", action="store_true", help="show what the BMS runs, change nothing")
    ap.add_argument("--wait", action="store_true",
                    help="after the reboot, wait until the new image runs and has confirmed itself")
    ap.add_argument("--retries", type=int, default=8, help="silent ACK timeouts tolerated per window")
    ap.add_argument("--max-rate", type=float, metavar="FPS",
                    help="pace data frames to at most FPS per second (bus-load budget)")
    args = ap.parse_args(argv)

    kwargs = {"bitrate": args.bitrate} if args.interface == "slcan" else {}
    bus = can.Bus(interface=args.interface, channel=args.channel,
                  can_filters=[{"can_id": bms_ota.RSP_ID, "can_mask": 0x7FF}], **kwargs)
    try:
        client = bms_ota.OtaClient(bus, retries=args.retries, max_frame_rate=args.max_rate)
        try:
            info = client.info()
        except bms_ota.OtaTimeout:
            print("no answer from the BMS (is it running, and is this the right channel?)", file=sys.stderr)
            return 2
        print(f"BMS: {info}")
        if args.info:
            return 0

        try:
            image = pick_image(args, info)
            header = bms_image.check_layout(image)
        except (OSError, ValueError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 3
        print(f"sending v{header.version_str} ({len(image)} bytes) to slot {'AB'[info.inactive_slot]}")

        t0 = time.monotonic()
        try:
            result = client.update(image, progress=lambda d, t, r: progress_bar(d, t, r, t0))
        except bms_ota.OtaRefused as e:
            print(f"\nrefused: {e}", file=sys.stderr)
            return 1
        except bms_ota.OtaTimeout as e:
            print(f"\nfailed: {e}", file=sys.stderr)
            return 2
        print(f"\nverified and staged by the BMS in {result.seconds:.1f} s "
              f"({result.rate / 1024:.1f} KiB/s, {result.retransmissions} retransmissions); it is rebooting")

        if args.wait:
            new = client.wait_for(lambda i: i.running_version == header.image_version, timeout=30)
            if new is None:
                print("the new image did not come up (rolled back?)", file=sys.stderr)
                return 2
            print(f"BMS: {new}")
            done = client.wait_for(lambda i: i.confirmed, timeout=60)
            if done is None:
                print("the new image has not confirmed itself", file=sys.stderr)
                return 2
            print(f"BMS: {done}")
        return 0
    finally:
        bus.shutdown()


if __name__ == "__main__":
    sys.exit(main())
