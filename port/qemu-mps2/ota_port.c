/*
 * ota_port.c - Platform side of the update logic on the MPS2-AN385.
 */
#include "ota_port.h"

#include <stddef.h>

#include "bms_hal.h"
#include "bms_keys.h"
#include "crc32.h"
#include "flash_hal.h"
#include "memory_map.h"
#include "port_image.h"

static const boot_meta_store_t s_store = { { MM_META_COPY0, MM_META_COPY1 } };

/* Fault injection: a simulated power cut stops the CPU right there, like a
 * brown-out reset - no log flush, no cleanup. Flash keeps whatever was
 * already programmed (QEMU keeps the RAM that plays flash across a reset). */
static void power_cut(void)
{
    hal_system_reset();
}

void ota_port_flash_init(void)
{
    flash_emul_init(0u, MM_FLASH_BASE, MM_FLASH_SIZE);
    (void)flash_protect(MM_BOOT_BASE, MM_BOOT_SIZE);
    /* An image must never erase itself: it executes from this flash. */
    (void)flash_protect(MM_APP_BASE, MM_SLOT_SIZE);
    flash_emul_set_power_loss_handler(power_cut);
}

uint8_t ota_port_running_slot(void)
{
    return (MM_APP_BASE == MM_SLOT_A_BASE) ? BOOT_SLOT_A : BOOT_SLOT_B;
}

uint32_t ota_port_running_version(void)
{
    return port_running_image()->image_version;
}

uint32_t ota_port_slot_base(uint8_t slot)
{
    return (slot == BOOT_SLOT_A) ? MM_SLOT_A_BASE : MM_SLOT_B_BASE;
}

uint32_t ota_port_slot_size(void)
{
    return MM_SLOT_SIZE;
}

const boot_meta_store_t *ota_port_meta_store(void)
{
    return &s_store;
}

image_policy_t ota_port_image_policy(uint8_t slot, uint32_t min_version)
{
    const image_policy_t p = {
        .slot_base = ota_port_slot_base(slot),
        .slot_size = MM_SLOT_SIZE,
        .ram_start = MM_RAM_BASE,
        .ram_end = MM_RAM_BASE + MM_RAM_SIZE,
        .min_version = min_version,
        .public_key = g_bms_signing_pubkey,
    };
    return p;
}

bool ota_port_boot_info(boot_info_t *info)
{
    *info = *(const volatile boot_info_t *)MM_SHARED_BASE;
    return info->magic == BOOT_INFO_MAGIC && info->crc32 == crc32(info, offsetof(boot_info_t, crc32));
}
