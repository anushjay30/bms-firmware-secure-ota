/*
 * boot_info.h - What the bootloader tells the application about this boot.
 *
 * Written by the bootloader into the SHARED RAM mailbox just before it jumps;
 * read by the application at startup (it logs it and reports it over CAN).
 * RAM, not flash: it describes this boot only and costs no flash wear. The
 * metadata in flash stays the single source of truth for decisions.
 */
#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include <stdint.h>

#define BOOT_INFO_MAGIC 0x544F4F42u /* bytes 'B','O','O','T' */

typedef struct {
    uint32_t magic;
    uint8_t  slot;          /* slot started (BOOT_SLOT_A/B)                        */
    uint8_t  reason;        /* boot_reason_t                                        */
    uint8_t  attempt;       /* trial boot number (1..BOOT_MAX_TRIAL_BOOTS) or 0     */
    uint8_t  other_slot;    /* slot rolled back from / ignored, or BOOT_SLOT_NONE   */
    uint32_t image_version; /* of the image started                                 */
    uint32_t min_version;   /* anti-rollback floor after this boot                  */
    uint32_t crc32;         /* CRC-32 of the fields above                           */
} boot_info_t;

#endif /* BOOT_INFO_H */
