# keys/ - firmware signing keys (development only)

Everything in this directory except this README is ignored by git
(`.gitignore`: `keys/*`, `*.pem`). **Never commit a private key.**

| File | What | Created by |
|---|---|---|
| `dev_ed25519.pem` | Ed25519 private key, PKCS#8 PEM, unencrypted, mode 0600 | `tools/keygen.py`, automatically on the first `make TARGET=qemu` |
| `dev_ed25519.pub.pem` | the matching public key | same |

The build turns the public key into `build/qemu/gen/signing_pubkey.c`, which is
linked into the bootloader (refuses to boot anything else) and the application
(refuses to stage an update the bootloader would refuse). The private key is
only used by `tools/sign_image.py`.

Each checkout generates its own key, so images signed on one machine do not
boot a bootloader built on another. Regenerating the key (`tools/keygen.py
--force`) invalidates every image signed with the old one.

## What production would do differently

- The private key is generated inside an **HSM** (or a cloud KMS) and never
  leaves it. A **signing server** in front of it accepts image digests from an
  authenticated release pipeline, logs every request, and returns signatures.
  No developer machine and no CI runner ever holds the key.
- Only the public key is exported - which is all the firmware build needs
  (`tools/keygen.py --export-c` accepts a public-key PEM for that reason).
- Separate keys for development and production, so a development image can
  never boot on a production ECU.
- A plan for key rotation and revocation, e.g. several public key slots in the
  bootloader and a signed "revoke key N" record, because a bootloader in the
  field cannot easily be replaced.
