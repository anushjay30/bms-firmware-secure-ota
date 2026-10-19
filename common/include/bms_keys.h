/*
 * bms_keys.h - The firmware signing public key.
 *
 * Compiled into the bootloader (which refuses to start an image not signed
 * with the matching private key) and into the application (which refuses to
 * stage an update that would not boot). The definition is generated at build
 * time by tools/keygen.py --export-c from the development key in keys/.
 */
#ifndef BMS_KEYS_H
#define BMS_KEYS_H

#include <stdint.h>

#include "bms_image.h"

extern const uint8_t g_bms_signing_pubkey[BMS_IMAGE_PUBKEY_SIZE];
extern const char g_bms_signing_key_id[]; /* first 4 bytes of SHA-256(key), hex: for logs */

#endif /* BMS_KEYS_H */
