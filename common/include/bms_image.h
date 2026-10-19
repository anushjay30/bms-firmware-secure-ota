/*
 * bms_image.h - Application image format and its verification.
 *
 * Every slot starts with this 256-byte header; the image proper (the
 * "payload": vector table, code, constants, initial values of .data) follows
 * it directly, so the vector table sits at slot + 0x100, aligned the way VTOR
 * requires. tools/bms_image.py mirrors this layout; tools/sign_image.py fills
 * in the CRC, the version and the signature after the link.
 *
 *   off   size  field
 *   0x00     4  magic           'B','M','S','I'
 *   0x04     2  header_size     256
 *   0x06     2  header_version  BMS_IMAGE_HEADER_VERSION
 *   0x08     4  image_version   major << 16 | minor << 8 | patch (compared as an integer)
 *   0x0C     4  image_size      payload bytes after the header
 *   0x10     4  image_crc32     CRC-32 of the payload
 *   0x14     4  load_address    slot base the image was linked for (it runs in place)
 *   0x18     4  vector_table    load_address + 0x100: where VTOR must point
 *   0x1C     4  flags           reserved, 0
 *   0x20   160  reserved        zero
 *   0xC0    64  signature       Ed25519 over SHA-512(header bytes 0x00..0xBF || payload)
 *
 * The signature lives inside the header it protects, so the signed bytes are
 * not one contiguous range. Hence hash-then-sign (as MCUboot does): SHA-512
 * over both parts, then a standard (pure) Ed25519 signature over that 64-byte
 * digest - which Python's `cryptography` package can produce. Every field
 * above the signature is covered, so none of them can be changed (version,
 * size, CRC, load address) without invalidating it.
 */
#ifndef BMS_IMAGE_H
#define BMS_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define BMS_IMAGE_MAGIC          0x49534D42u /* bytes 'B','M','S','I' (little-endian) */
#define BMS_IMAGE_HEADER_SIZE    256u
#define BMS_IMAGE_HEADER_VERSION 2u
#define BMS_IMAGE_SIGNED_HEADER  0xC0u       /* header bytes covered by the signature */
#define BMS_IMAGE_PUBKEY_SIZE    32u
#define BMS_IMAGE_SIG_SIZE       64u

#define BMS_VERSION(major, minor, patch) (((uint32_t)(major) << 16) | ((uint32_t)(minor) << 8) | (uint32_t)(patch))

typedef struct {
    uint32_t magic;
    uint16_t header_size;
    uint16_t header_version;
    uint32_t image_version;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t load_address;
    uint32_t vector_table;
    uint32_t flags;
    uint8_t  reserved[BMS_IMAGE_SIGNED_HEADER - 0x20u];
    uint8_t  signature[BMS_IMAGE_SIG_SIZE];
} bms_image_header_t;

_Static_assert(sizeof(bms_image_header_t) == BMS_IMAGE_HEADER_SIZE, "header must be exactly 256 bytes");
_Static_assert(offsetof(bms_image_header_t, signature) == BMS_IMAGE_SIGNED_HEADER,
               "signature must follow the signed part of the header");

/* Checks run cheapest first, and the first failure is reported. */
typedef enum {
    IMAGE_OK = 0,
    IMAGE_ERR_EMPTY,     /* no image: magic missing (erased slot)                      */
    IMAGE_ERR_HEADER,    /* unknown header size/version                                */
    IMAGE_ERR_SLOT,      /* linked for a different slot (load address / vector table)  */
    IMAGE_ERR_SIZE,      /* payload empty or larger than the slot                      */
    IMAGE_ERR_VERSION,   /* older than the anti-rollback minimum                       */
    IMAGE_ERR_CRC,       /* payload CRC mismatch: corrupted or incompletely written    */
    IMAGE_ERR_SIGNATURE, /* not signed by our key, or modified after signing           */
    IMAGE_ERR_VECTORS    /* vector table implausible (initial SP / reset handler)       */
} image_result_t;

typedef struct {
    uint32_t       slot_base;   /* address the slot is mapped at (header start)           */
    uint32_t       slot_size;
    uint32_t       ram_start;   /* the initial stack pointer must lie in (ram_start,      */
    uint32_t       ram_end;     /*   ram_end]                                              */
    uint32_t       min_version; /* anti-rollback floor                                     */
    const uint8_t *public_key;  /* Ed25519 key the image must be signed with (32 bytes)   */
} image_policy_t;

/* Verify the image in a slot. `mem` points at the slot's contents (on target:
 * the slot address itself, the image runs in place; in unit tests: a buffer
 * standing in for it). On success *header receives a copy of the header. */
image_result_t bms_image_verify(const uint8_t *mem, const image_policy_t *policy, bms_image_header_t *header);

const char *bms_image_result_name(image_result_t r);

#endif /* BMS_IMAGE_H */
