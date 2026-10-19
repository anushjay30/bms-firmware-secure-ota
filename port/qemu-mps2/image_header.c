/*
 * image_header.c - The 256-byte header at the start of this image's slot.
 *
 * The linker places it at slot + 0 and the build fills in what is known at
 * link time: sizes and addresses. tools/sign_image.py then stamps the payload
 * CRC, the release version and the Ed25519 signature into the binary - so the
 * bytes in flash differ from this initialiser. Read them through
 * port_running_image() (a volatile read of the flash), never through
 * g_image_header itself: the compiler knows this initialiser and may
 * constant-fold any field you read from it.
 */
#include "port_image.h"

#include "memory_map.h"

#ifndef BMS_IMAGE_VERSION
#define BMS_IMAGE_VERSION BMS_VERSION(1, 0, 0)
#endif

/* Linker script symbol whose *address* is the payload size. */
extern const uint8_t __image_size[];

__attribute__((section(".image_header"), used))
const bms_image_header_t g_image_header = {
    .magic          = BMS_IMAGE_MAGIC,
    .header_size    = BMS_IMAGE_HEADER_SIZE,
    .header_version = BMS_IMAGE_HEADER_VERSION,
    .image_version  = BMS_IMAGE_VERSION, /* default; the signing tool sets the release version */
    .image_size     = (uint32_t)__image_size,
    .image_crc32    = 0u,                /* stamped by the signing tool */
    .load_address   = MM_APP_BASE,
    .vector_table   = MM_APP_VECTORS,
    .flags          = 0u,
    .reserved       = { 0 },
    .signature      = { 0 },             /* stamped by the signing tool */
};

const volatile bms_image_header_t *port_running_image(void)
{
    return (const volatile bms_image_header_t *)MM_APP_BASE;
}

char port_running_slot_name(void)
{
    return (MM_APP_BASE == MM_SLOT_A_BASE) ? 'A' : 'B';
}
