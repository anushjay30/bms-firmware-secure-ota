/*
 * memory_map.h - Memory layout of the BMS on the MPS2-AN385 (Cortex-M3).
 *
 * Single source of truth: this header is included by the C code AND by the
 * linker script (mps2_an385.ld.S is run through the C preprocessor), so the
 * addresses the code uses can never drift from the ones the linker uses.
 * Hence plain integer literals only - no 'U' suffixes or casts, which the
 * linker script language would not understand.
 *
 * The AN385 FPGA image has no flash. Its 4 MB "ZBT SSRAM1" at 0x00000000 is
 * the memory the core boots from (on the real board the MCC copies the image
 * there from the SD card), so it plays the role of flash here. Data lives in
 * the 4 MB "ZBT SSRAM2/3" at 0x20000000.
 *
 *   0x0000_0000 +----------------------+
 *               | BOOT        64 KB    |  bootloader: picks a slot, verifies it,
 *               |                      |  jumps to it (write-protected)
 *   0x0001_0000 +----------------------+
 *               | META        64 KB    |  boot metadata: active slot, trial-boot
 *               |                      |  state, anti-rollback floor
 *   0x0002_0000 +----------------------+
 *               | SLOT_A       1 MB    |  application image A: 256 B header,
 *               |                      |  vector table, code, .data init values
 *   0x0012_0000 +----------------------+
 *               | SLOT_B       1 MB    |  application image B (same layout)
 *   0x0022_0000 +----------------------+
 *               | unused   1.875 MB    |
 *   0x0040_0000 +----------------------+
 *
 *   0x2000_0000 +----------------------+
 *               | RAM    4 MB - 4 KB   |  .data, .bss, task stacks; MSP stack at
 *               |                      |  the top (grows down)
 *   0x203F_F000 +----------------------+
 *               | SHARED       4 KB    |  bootloader -> app mailbox (boot_info.h),
 *               |                      |  not touched by either startup code
 *   0x2040_0000 +----------------------+
 */
#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

#define MM_BOOT_BASE          0x00000000
#define MM_BOOT_SIZE          0x00010000
#define MM_META_BASE          0x00010000
#define MM_META_SIZE          0x00010000
#define MM_SLOT_A_BASE        0x00020000
#define MM_SLOT_B_BASE        0x00120000
#define MM_SLOT_SIZE          0x00100000

#define MM_RAM_BASE           0x20000000
#define MM_RAM_SIZE           0x003FF000
#define MM_SHARED_BASE        0x203FF000
#define MM_SHARED_SIZE        0x00001000

/* The whole 4 MB SSRAM1, as the flash HAL sees it: BOOT is write-protected,
 * the rest is erased/programmed with flash rules (4 KB pages, 8-byte units). */
#define MM_FLASH_BASE         0x00000000
#define MM_FLASH_SIZE         0x00400000

/* The two metadata copies (boot_meta.h), inside META: each in its own 4 KB
 * erase page, so erasing one can never touch the other. The rest of META is
 * spare (e.g. for a future copy-on-write log). */
#define MM_META_COPY0         0x00010000
#define MM_META_COPY1         0x00011000

/* Stack for Reset_Handler/main() before the scheduler starts, and for every
 * interrupt handler afterwards (FreeRTOS tasks have their own stacks). */
#define MM_MSP_STACK_SIZE     0x00001000

/* The bootloader runs before the application and uses the same RAM; its
 * stack (top of RAM) mostly serves Ed25519 verification, which needs ~3 KB. */
#define MM_BOOT_STACK_SIZE    0x00002000

/*
 * Every application image starts with a 256-byte header (bms_image.h),
 * followed by its vector table. 256 is not arbitrary: VTOR requires the table
 * to be aligned to its size rounded up to a power of two. 16 system exceptions
 * + 32 AN385 interrupts = 48 words = 192 bytes -> 256-byte alignment.
 */
#define MM_IMAGE_HEADER_SIZE  0x00000100

/* Which slot this build is linked to run from: the Makefile links the app
 * twice, defining BMS_APP_SLOT_B for the slot B build.
 * Images execute in place, so an image linked for slot A only runs from A. */
#if defined(BMS_APP_SLOT_B)
#define MM_APP_BASE           MM_SLOT_B_BASE
#else
#define MM_APP_BASE           MM_SLOT_A_BASE
#endif
#define MM_APP_VECTORS        (MM_APP_BASE + MM_IMAGE_HEADER_SIZE)

#endif /* MEMORY_MAP_H */
