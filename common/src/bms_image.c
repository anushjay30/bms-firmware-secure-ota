#include "bms_image.h"

#include <stdbool.h>
#include <string.h>

#include "crc32.h"
#include "monocypher-ed25519.h"

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Structural checks on the header alone: cheap, and they make every later
 * step safe (e.g. the size bounds the CRC and hash to the slot). */
static image_result_t check_header(const bms_image_header_t *h, const image_policy_t *p)
{
    if (h->magic != BMS_IMAGE_MAGIC) {
        return IMAGE_ERR_EMPTY;
    }
    if (h->header_size != BMS_IMAGE_HEADER_SIZE || h->header_version != BMS_IMAGE_HEADER_VERSION) {
        return IMAGE_ERR_HEADER;
    }
    /* Execute-in-place: an image linked for slot A is full of absolute slot-A
     * addresses and would crash if started from slot B. */
    if (h->load_address != p->slot_base || h->vector_table != p->slot_base + BMS_IMAGE_HEADER_SIZE) {
        return IMAGE_ERR_SLOT;
    }
    if (h->image_size == 0u || h->image_size > p->slot_size - BMS_IMAGE_HEADER_SIZE) {
        return IMAGE_ERR_SIZE;
    }
    return IMAGE_OK;
}

/* Last line of defence against a signed-but-broken build: the core would
 * load this SP and jump to this address. */
static image_result_t check_vectors(const uint8_t *mem, const bms_image_header_t *h, const image_policy_t *p)
{
    const uint8_t *vectors = mem + BMS_IMAGE_HEADER_SIZE;
    const uint32_t sp = get_u32(&vectors[0]);
    const uint32_t reset = get_u32(&vectors[4]);
    const uint32_t code_start = h->vector_table;
    const uint32_t code_end = h->vector_table + h->image_size;

    if (sp <= p->ram_start || sp > p->ram_end || (sp & 0x7u) != 0u) {
        return IMAGE_ERR_VECTORS; /* AAPCS wants the stack 8-byte aligned */
    }
    if ((reset & 1u) == 0u || (reset & ~1u) < code_start || (reset & ~1u) >= code_end) {
        return IMAGE_ERR_VECTORS; /* must be Thumb (bit 0) and inside the image */
    }
    return IMAGE_OK;
}

/*
 * Authenticity: was this exact header + payload signed with our key?
 * The header bytes hashed are those of the copy `h` that every decision is
 * made from (size, version, vector table) - so what is checked is what is
 * used, even if the flash were to change underneath us.
 */
static bool signature_valid(const uint8_t *mem, const bms_image_header_t *h, const uint8_t *public_key)
{
    if (public_key == NULL) {
        return false; /* no key, no trust: never skip the check */
    }
    crypto_sha512_ctx ctx;
    uint8_t digest[64];
    crypto_sha512_init(&ctx);
    crypto_sha512_update(&ctx, (const uint8_t *)h, BMS_IMAGE_SIGNED_HEADER);
    crypto_sha512_update(&ctx, mem + BMS_IMAGE_HEADER_SIZE, h->image_size);
    crypto_sha512_final(&ctx, digest);
    /* Standard Ed25519 verification (RFC 8032) of the signature over the
     * digest; Monocypher also rejects non-canonical (malleable) signatures. */
    return crypto_ed25519_check(h->signature, public_key, digest, sizeof digest) == 0;
}

image_result_t bms_image_verify(const uint8_t *mem, const image_policy_t *policy, bms_image_header_t *header)
{
    bms_image_header_t h;
    memcpy(&h, mem, sizeof h); /* flash may be unaligned for a struct in unit tests */

    image_result_t r = check_header(&h, policy);
    if (r != IMAGE_OK) {
        return r;
    }
    /* Anti-rollback before the expensive checks: an old image is refused no
     * matter how valid its signature is - old versions are exactly the ones
     * with known vulnerabilities. */
    if (h.image_version < policy->min_version) {
        return IMAGE_ERR_VERSION;
    }
    /* Integrity: cheap, catches flash corruption and interrupted writes with
     * a clear diagnosis. It is NOT a security check - anyone can compute a
     * CRC for a modified image; that is what the signature is for. */
    if (crc32(mem + BMS_IMAGE_HEADER_SIZE, h.image_size) != h.image_crc32) {
        return IMAGE_ERR_CRC;
    }
    if (!signature_valid(mem, &h, policy->public_key)) {
        return IMAGE_ERR_SIGNATURE;
    }
    r = check_vectors(mem, &h, policy);
    if (r != IMAGE_OK) {
        return r;
    }
    if (header != NULL) {
        *header = h;
    }
    return IMAGE_OK;
}

const char *bms_image_result_name(image_result_t r)
{
    switch (r) {
    case IMAGE_OK:            return "ok";
    case IMAGE_ERR_EMPTY:     return "empty (no image)";
    case IMAGE_ERR_HEADER:    return "bad header";
    case IMAGE_ERR_SLOT:      return "linked for another slot";
    case IMAGE_ERR_SIZE:      return "bad size";
    case IMAGE_ERR_VERSION:   return "version below minimum (rollback)";
    case IMAGE_ERR_CRC:       return "CRC mismatch";
    case IMAGE_ERR_SIGNATURE: return "bad signature";
    case IMAGE_ERR_VECTORS:   return "bad vector table";
    default:                  return "?";
    }
}
