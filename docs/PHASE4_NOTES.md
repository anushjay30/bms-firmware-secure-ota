# Phase 4 - secure bootloader, A/B updates over CAN: design notes

Companion to the README's reference sections. This is the "why", the interview
prep and the résumé material for the bootloader / OTA work.

## Design decisions and the reasoning behind them

**A separate bare-metal bootloader, not a stub in the app.** The thing that
decides whether to trust an image must not be the image. The bootloader is its
own linked binary in a 64 KB region at address 0, with no FreeRTOS; on real
hardware it lives in write-protected flash and a mask-ROM root of trust would
verify *it*. It shares the verification code (`bms_image.c`), the metadata
(`boot_meta.c`), the flash HAL and the polled UART with the app, so there is
one implementation of "is this image good", used identically at boot and
before staging.

**Hash-then-sign Ed25519.** The signature sits in the header it protects, so
the signed bytes are not contiguous; signing a SHA-512 digest of
`header[0..0xBF] ‖ payload` (as MCUboot does) sidesteps that and lets the
signer be Python's `cryptography` while the verifier is Monocypher - two
independent implementations of standard RFC 8032 Ed25519, cross-checked by the
test vectors. Ed25519 over RSA: small key (32 B public, compiled in), small
signature (64 B), fast verify, no parameter choices to get wrong, constant-time
by construction.

**CRC-32 *and* a signature.** They answer different questions. CRC gives a
cheap, clear "the transfer/flash is corrupted" before the expensive check, and
the check order is cheapest-first so a blank or truncated slot is rejected in
microseconds. The signature answers "was this authorised" - a CRC proves
nothing against an attacker, and a test (`a_recomputed_crc_does_not_fool_the_signature`)
makes that explicit.

**Direct-XIP A/B (two linked images), not copy/swap or PIC.** The app executes
in place and is full of absolute addresses, so it is linked once per slot. The
updater sends the image for the inactive slot, and switching versions is a
metadata change, never a copy - instant, and rollback costs nothing. The price,
two binaries per release and a tool that asks which slot runs, is small;
copy-to-primary doubles every update's flash writes and needs its own
power-fail-safe swap log, and PIC on an MCU with a vector table of absolute
addresses is a lot of pain for little gain. The load address is in the signed
header, so an image can only run where it belongs.

**Two-copy metadata with sequence + CRC.** A write only ever touches the copy
that is *not* the current state, so a power cut during erase or program can
only damage the stale copy; the next boot takes the intact one. The CRC is the
last field programmed, so a record is valid only once fully written. This is
the classic A/B (ping-pong) journalling pattern, and the unit tests cut power
at every byte of a write to prove no state is ever lost or half-read.

**Trial boot + rollback driven by a counter persisted before the jump.** A new
image is on trial until it proves itself; the bootloader bumps the attempt
counter and writes it *before* starting the image, so an image that hangs or
crashes still burns an attempt and is rolled back after three. Fatal handlers
reset the ECU rather than halt, which is what makes "an image that crashes
rolls itself back" work with no outside help.

**Confirmation = a real health check, not a heartbeat.** The app confirms only
after every task has checked in with the watchdog and no fault has latched or
been active for a configurable window. It is the supervisor - the highest-
priority task, the one that already judges liveness - that makes the call, and
the update task (the single flash owner) performs the write.

**The contactor state machine owns the safety interlock.** "Refuse to update
unless the contactors are open" is a check-then-act hazard if two tasks do it.
So the state machine answers the lock request *inside the same step* that
handles CLOSE commands: the lock is granted only in OPEN, a CLOSE in the same
step wins, and while the lock is held CLOSE is refused. The update code can
never open a contactor; it can only hold the existing open state. Monitoring
runs underneath because the update task is the lowest real task and CAN RX only
forwards update frames.

**One task owns the flash.** No second writer, so no lock on the flash or the
metadata. It runs below everything safety-related, so a 7-million-instruction
signature check or a page erase never delays a fault reaction.

**Emulated flash with real semantics + fault injection.** QEMU's "flash" is
RAM; the HAL enforces page erase, erase-before-write, program-only-clears-bits,
8-byte units and write protection, so the logic is what an STM32 driver would
need. The fault-injection hook (fail a write / cut power after N bytes) is what
lets the power-loss tests exist at all.

**Investigated QEMU reset empirically.** Rather than assume, I measured that
QEMU restores any `-kernel`/`-device loader` image on every system reset but
keeps everything else. So only the bootloader is loaded that way; slots and
metadata are programmed once through the GDB stub, like an SWD programmer, and
then behave like flash across `SYSRESETREQ`. Documented with the measurement
table.

## Interview questions I should be ready to answer

**Q. Walk me through what happens from power-on to the application running.**
The core loads MSP and PC from address 0 (the bootloader's vector table; VTOR
is 0 at reset). The bootloader reads both metadata copies and picks the newest
valid one; decides the slot (pending update → trial; trial over its attempt
limit → rollback; else the active slot, or the other as fallback if it is
broken); verifies that slot's image (magic, load address, size, version ≥
floor, CRC-32, Ed25519, vector-table sanity); persists any metadata change
*before* jumping; writes a mailbox for the app; then disables interrupts, stops
SysTick, clears pending NVIC, sets VTOR to slot+0x100, loads the app's MSP and
branches to its reset handler. The app re-sets VTOR and MSP itself, inits
.data/.bss and starts FreeRTOS.

**Q. Why hash-then-sign instead of signing the image directly?** The signature
has to live in the image header (nowhere else to put it on an XIP image), so
the bytes being signed aren't contiguous - header-before-signature plus
payload. Hashing both parts into one SHA-512 digest and signing that solves it,
and it is exactly the Ed25519ph-style construction MCUboot uses. It also means
the signer only needs the digest, which is what a production HSM/signing-server
flow wants.

**Q. How does the update survive power loss mid-write?** Two places matter.
During the *transfer*, the inactive slot is being erased/programmed; if power
cuts, that slot is incomplete but the running slot and the metadata are
untouched, so the old image just boots again - the half-written slot is never
marked pending. During the *commit* (writing "pending = B" to metadata), the
two-copy scheme means the write goes to the older copy; a cut leaves it torn
(bad CRC) and the newest-valid copy is still the pre-commit state. Only when the
CRC - the last word - lands does the new state become current. Tested by
injecting a power cut at every byte offset.

**Q. What stops someone flashing malicious firmware over CAN?** They can run
the protocol, but the image is staged only if it passes the exact check the
bootloader will repeat: a valid Ed25519 signature over the image, by the key
whose public half is compiled into both. Without the private key you cannot
produce that, and tampering (even fixing the CRC) breaks the signature. What it
does *not* stop: someone with the key, physical/SWD access, or a denial-of-
service on the unauthenticated protocol - see the threat model.

**Q. Why not just check a version number for rollback protection?** The version
is in the signed header, so it cannot be forged - but an attacker could replay
an old, validly-signed, vulnerable release. The anti-rollback *floor* (stored
in metadata, raised when an image confirms) refuses anything below it,
regardless of signature. Enforced both in the bootloader and at the Start of an
update. A test relabels an old image as new and shows the signature check
catches it; another announces a fake version in the (unsigned) Start request
and shows the signed header is what counts.

**Q. How do you guarantee you never brick the device?** Three layers: a new
image is on trial and rolls back after 3 unconfirmed boots (covering hangs and
crashes, because fatal handlers reset and each reset costs an attempt); the
bootloader falls back to the other slot if the active image fails verification;
and if neither slot is valid it stays in the bootloader and says so on the log
UART, where a production design would offer a recovery path. The rollback target
is protected: no update is accepted while the running image is still on trial.

**Q. Why is the update the lowest-priority task, and why does that matter for
safety?** Flash erases/writes stall and a signature check is millions of
instructions; if that ran at or above the monitor/fault tasks it would delay
cell monitoring and fault reactions. At the lowest priority it only gets spare
CPU, so a firmware update never compromises the BMS's actual job - which the
"fault during a transfer is still caught within budget" scenario proves.

**Q. The contactors-open check - how do you avoid a race?** The contactor state
machine is the single owner of contactor state, so it makes the decision,
inside one step, together with any CLOSE in that step. The update task asks for
a lock; the SM grants it only in OPEN and then refuses CLOSE until release. No
other task ever reads "are we open" and acts on a stale answer. A unit test
fires a CLOSE and the lock request in the same step and checks the close wins
and the lock is refused.

**Q. You tested on an emulator - what's different on real silicon?** See
"What would change on real hardware" in the README: a real flash-controller
driver behind the same HAL (erase timing, ECC, read-while-write across banks),
the bootloader in write-protected flash with debug lock and ideally a ROM root
of trust, the anti-rollback counter in OTP, a hardware watchdog, a real CAN
controller with arbitration (so the "lowest IDs" actually yield) and UDS, and
keys in an HSM. The logic is written to port: addresses live in one header, the
platform contract is one interface.

**Q. Why Monocypher?** Small, auditable, permissively licensed, no dynamic
allocation, usable bare-metal in the bootloader, and its optional module does
standard SHA-512 Ed25519 that interoperates with mainstream tools. The cost is
~12 KB of flash and a ~2.7 KB stack peak for a verify, which the budget has room
for.

**Q. How fast is an update, and what dominates?** Over the emulated slcan link
a 44 KB image transfers and verifies in ~20 s on a native host (the UART, not
the CPU, is the bottleneck). Verification itself is ~7 M instructions (≈0.3 s on
a 25 MHz M3), dominated by Ed25519's field arithmetic. On real CAN FD at a sane
bus load it would be seconds.

## Résumé bullet points

- Built a secure A/B firmware-update system for an ARM Cortex-M3 battery
  management ECU (FreeRTOS, bare-metal bootloader, QEMU): **Ed25519-signed
  images** (Monocypher + Python `cryptography`, RFC 8032), **CRC-32** integrity,
  **anti-rollback**, and **trial-boot with automatic rollback** - a bad or
  crashing image is reverted within 3 boots with no operator action.
- Designed **power-loss-safe boot metadata** (two flash pages, sequence +
  CRC-32, write-the-older-copy journalling) and a **flash HAL with fault
  injection**; unit tests cut power at *every byte* of a metadata write and the
  device always recovers the old or new state - never a corrupt one.
- Implemented **firmware update over CAN** (windowed transfer, cumulative ACKs,
  go-back-N retransmission, timeouts) with a **safety interlock**: the contactor
  state machine grants the update lock only while the HV contactors are open and
  refuses to close them during a transfer, and cell monitoring/fault reaction
  continue throughout (verified: an overtemperature fault mid-update still trips
  within its 300 ms budget).
- Delivered it test-first: **179 unit tests** (Unity, ASan/UBSan - image
  verification, RFC 8032 vectors, flash semantics, A/B state machine,
  power-cut sweeps) plus **24 hardware-free end-to-end scenarios** over virtual
  CAN (valid update, corruption, wrong key, downgrade, rollback, power loss,
  closed-contactor refusal), all in CI. Bootloader **15.6 KB**; image verify
  **≈7 M instructions**; app **44 KB** flash / 30 KB RAM, fully static (no heap).

## Headline numbers

| | |
|---|---|
| Bootloader size | 15.6 KB of 64 KB (24 %) |
| Application size | 44 KB flash, 30 KB static RAM per slot, zero heap |
| Image verify cost | ≈7.0 M instructions (CRC-32 + SHA-512 + Ed25519), ≈0.3 s @ 25 MHz |
| Update task stack peak | 684 words (2.7 KB), full transfer + verify |
| Trial boots before rollback | 3 |
| Unit tests | 179 (17 suites) |
| Offline consistency tests | 18 |
| End-to-end scenarios | 24 (14 BMS + 10 update) |
| Update throughput (native slcan) | 44 KB in ~20 s, UART-bound |
