#include "crc8.h"

#define CRC8_SAE_J1850_POLY 0x1Du

/*
 * Bitwise implementation: 8 shifts per byte. For 8-byte CAN payloads this is
 * a few hundred cycles; a 256-byte lookup table would be faster but costs
 * flash. Either is fine at our message rates.
 */
uint8_t crc8_sae_j1850_update(uint8_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8u; bit++) {
            if ((crc & 0x80u) != 0u) {
                crc = (uint8_t)((crc << 1) ^ CRC8_SAE_J1850_POLY);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint8_t crc8_sae_j1850(const uint8_t *data, size_t len)
{
    return (uint8_t)(crc8_sae_j1850_update(CRC8_SAE_J1850_INIT, data, len) ^ CRC8_SAE_J1850_XOROUT);
}
