#!/usr/bin/env python3
"""Program, erase and read QEMU's emulated flash through its GDB stub.

Why not just -kernel / -device loader for everything? QEMU records every image
given that way as a "ROM blob" and copies it back into guest memory on every
system reset (README: "QEMU and reset" has the measurements). An application
slot loaded like that behaves like mask ROM: an update the firmware writes
into it is silently undone by the next reboot. So run_qemu.sh loads only the
bootloader with -kernel - on a real part it sits in write-protected flash, so
restoring it is harmless - and programs the slots and the metadata once, at
power-on, through the GDB stub: the emulator's equivalent of an SWD
programmer on the production line. From then on only the firmware's flash
HAL changes them, and they survive resets like real flash.

Usage (operations run in this order: erase, write, dump, reset, resume/halt):
  qemu_flash.py [--host H] --port N [--erase ADDR:LEN]... [--write ADDR:FILE]...
                [--dump ADDR:LEN:FILE]... [--reset] [--resume]

Connecting stops the guest; without --resume it stays stopped (make debug).
"""
from __future__ import annotations

import argparse
import socket
import sys
import time
from pathlib import Path

ERASED = 0xFF
# QEMU's gdbstub accepts packets up to 4096 bytes (MAX_PACKET_LENGTH); an
# 'M' packet carries two hex digits per byte plus "Maddr,len:".
CHUNK = 2000


class GdbStubError(RuntimeError):
    pass


class GdbStub:
    """Minimal GDB Remote Serial Protocol client: memory access, monitor
    commands and detach - all a flash programmer needs."""

    def __init__(self, host: str = "127.0.0.1", port: int = 1234, timeout: float = 10.0,
                 connect_timeout: float = 10.0) -> None:
        deadline = time.monotonic() + connect_timeout
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=timeout)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)  # QEMU may still be starting up
        # Every packet is a small write answered by a small write. With Nagle's
        # algorithm on, each one waits for the peer's delayed ACK (40 ms on
        # Linux): programming 2 MB took 45 s instead of under one.
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._buf = b""

    # ---- packet layer ------------------------------------------------------

    def _recv_byte(self) -> int:
        if not self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise GdbStubError("connection closed by QEMU")
            self._buf = chunk
        b, self._buf = self._buf[0], self._buf[1:]
        return b

    def _send_packet(self, payload: bytes) -> None:
        frame = b"$" + payload + b"#" + f"{sum(payload) & 0xFF:02x}".encode()
        for _ in range(3):
            self.sock.sendall(frame)
            ack = self._recv_byte()
            if ack == ord("+"):
                return
            if ack != ord("-"):
                raise GdbStubError(f"unexpected byte {ack:#x} instead of an ack")
        raise GdbStubError("packet rejected three times")

    def _recv_packet(self) -> bytes:
        while self._recv_byte() != ord("$"):
            pass
        payload = bytearray()
        while (b := self._recv_byte()) != ord("#"):
            payload.append(b)
        checksum = int(bytes([self._recv_byte(), self._recv_byte()]), 16)
        if checksum != sum(payload) & 0xFF:
            self.sock.sendall(b"-")
            return self._recv_packet()
        self.sock.sendall(b"+")
        return bytes(payload)

    def command(self, payload: str | bytes) -> bytes:
        self._send_packet(payload.encode() if isinstance(payload, str) else payload)
        return self._recv_packet()

    def _expect_ok(self, reply: bytes, what: str) -> None:
        if reply != b"OK":
            raise GdbStubError(f"{what}: QEMU replied {reply!r}")

    # ---- memory ------------------------------------------------------------

    def write(self, addr: int, data: bytes) -> None:
        for off in range(0, len(data), CHUNK):
            part = data[off:off + CHUNK]
            self._expect_ok(self.command(f"M{addr + off:x},{len(part):x}:{part.hex()}"),
                            f"write at {addr + off:#010x}")

    def read(self, addr: int, length: int) -> bytes:
        out = bytearray()
        while len(out) < length:
            n = min(CHUNK, length - len(out))
            reply = self.command(f"m{addr + len(out):x},{n:x}")
            if reply.startswith(b"E") and len(reply) == 3:
                raise GdbStubError(f"read at {addr + len(out):#010x}: {reply!r}")
            out += bytes.fromhex(reply.decode())
        return bytes(out)

    def erase(self, addr: int, length: int) -> None:
        """Fill with 0xFF: the erased state of NOR flash."""
        full = bytes([ERASED] * CHUNK).hex()  # encoded once, not per packet
        for off in range(0, length, CHUNK):
            n = min(CHUNK, length - off)
            data = full if n == CHUNK else full[:2 * n]
            self._expect_ok(self.command(f"M{addr + off:x},{n:x}:{data}"), f"erase at {addr + off:#010x}")

    # ---- execution control ------------------------------------------------

    def monitor(self, cmd: str) -> str:
        """Run a QEMU monitor command (what 'monitor <cmd>' does in GDB)."""
        self._send_packet(b"qRcmd," + cmd.encode().hex().encode())
        output = []
        while True:
            reply = self._recv_packet()
            if reply.startswith(b"O") and reply != b"OK":
                output.append(bytes.fromhex(reply[1:].decode()).decode(errors="replace"))
                continue
            if reply not in (b"OK", b""):
                raise GdbStubError(f"monitor {cmd}: {reply!r}")
            return "".join(output)

    def detach(self) -> None:
        """Detach: QEMU resumes the guest."""
        self._expect_ok(self.command("D"), "detach")
        self.close()

    def close(self) -> None:
        self.sock.close()

    def __enter__(self) -> "GdbStub":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def _int(text: str) -> int:
    return int(text, 0)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1234)
    ap.add_argument("--erase", action="append", default=[], metavar="ADDR:LEN",
                    help="fill a range with 0xFF (erased flash)")
    ap.add_argument("--write", action="append", default=[], metavar="ADDR:FILE",
                    help="program a binary file at ADDR")
    ap.add_argument("--dump", action="append", default=[], metavar="ADDR:LEN:FILE",
                    help="save a range to a file")
    ap.add_argument("--reset", action="store_true", help="system reset (as if the reset pin was pulsed)")
    ap.add_argument("--resume", action="store_true", help="let the guest run afterwards")
    args = ap.parse_args(argv)

    with GdbStub(args.host, args.port) as stub:
        for spec in args.erase:
            addr, length = (_int(x) for x in spec.split(":"))
            stub.erase(addr, length)
        for spec in args.write:
            addr_text, path = spec.split(":", 1)
            stub.write(_int(addr_text), Path(path).read_bytes())
        for spec in args.dump:
            addr_text, length_text, path = spec.split(":", 2)
            Path(path).write_bytes(stub.read(_int(addr_text), _int(length_text)))
        if args.reset:
            stub.monitor("system_reset")
        if args.resume:
            stub.detach()
    return 0


if __name__ == "__main__":
    sys.exit(main())
