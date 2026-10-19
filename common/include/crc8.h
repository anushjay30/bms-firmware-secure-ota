/*
 * crc8.h - CRC-8 SAE J1850 (poly 0x1D, init 0xFF, xorout 0xFF, no reflection).
 *
 * This is the 8-bit CRC that AUTOSAR's Crc_CalculateCRC8 implements and that
 * E2E profiles use to protect CAN payloads. Check value for "123456789": 0x4B.
 */
#ifndef CRC8_H
#define CRC8_H

#include <stddef.h>
#include <stdint.h>

#define CRC8_SAE_J1850_INIT   0xFFu
#define CRC8_SAE_J1850_XOROUT 0xFFu

/* Feed len bytes into a running CRC register (no final XOR). */
uint8_t crc8_sae_j1850_update(uint8_t crc, const uint8_t *data, size_t len);

/* Complete one-shot CRC over a buffer (init + final XOR applied). */
uint8_t crc8_sae_j1850(const uint8_t *data, size_t len);

#endif /* CRC8_H */
