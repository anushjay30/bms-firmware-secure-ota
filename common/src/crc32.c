#include "crc32.h"

/*
 * Nibble-at-a-time table: 16 entries (64 bytes) instead of the classic 256
 * (1 KB). Two lookups per byte - about 4x faster than bit-by-bit, at a size
 * that suits the bootloader, which checks a ~50 KB image on every boot.
 */
static const uint32_t s_table[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ s_table[crc & 0x0Fu];
        crc = (crc >> 4) ^ s_table[crc & 0x0Fu];
    }
    return ~crc;
}
