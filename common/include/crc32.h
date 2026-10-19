/*
 * crc32.h - CRC-32/ISO-HDLC, the CRC of zlib, PNG and Ethernet.
 *
 * Reflected polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF;
 * check value crc32("123456789") = 0xCBF43926. Python's zlib.crc32() gives
 * the same result, which is how the signing tool fills in the image header.
 *
 * Used for fast integrity checks (image payload, metadata records) - it
 * catches corruption, not tampering; authenticity is the signature's job.
 */
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Continue a CRC over more data: crc32_update(0, ...) starts a new one, and
 * crc32_update(crc32_update(0, a, n), b, m) equals the CRC of a followed by b. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

static inline uint32_t crc32(const void *data, size_t len)
{
    return crc32_update(0u, data, len);
}

#endif /* CRC32_H */
