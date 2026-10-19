/*
 * ota_port.h - What the update logic needs from the platform.
 *
 * Implemented by ports with a bootloader and flash (port/qemu-mps2:
 * BMS_PORT_HAS_OTA = 1). The application's update code (app/tasks/task_ota.c)
 * stays free of addresses and register names; this is the whole contract.
 */
#ifndef OTA_PORT_H
#define OTA_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_image.h"
#include "boot_info.h"
#include "boot_meta.h"

/* Set up the flash HAL for the application: write-protect the bootloader
 * and the running image, install the power-loss handler (fault injection). */
void ota_port_flash_init(void);

uint8_t  ota_port_running_slot(void);     /* BOOT_SLOT_A or BOOT_SLOT_B           */
uint32_t ota_port_running_version(void);  /* from the signed header in flash      */
uint32_t ota_port_slot_base(uint8_t slot);
uint32_t ota_port_slot_size(void);
const boot_meta_store_t *ota_port_meta_store(void);

/* Verification policy for an image in `slot` (addresses, RAM range, the
 * compiled-in public key), with the given anti-rollback floor. */
image_policy_t ota_port_image_policy(uint8_t slot, uint32_t min_version);

/* What the bootloader reported for this boot; false if the mailbox is
 * missing or corrupted (e.g. the image was started by a debugger). */
bool ota_port_boot_info(boot_info_t *info);

#endif /* OTA_PORT_H */
