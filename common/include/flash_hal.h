/*
 * flash_hal.h - Internal flash, as the bootloader and the application use it.
 *
 * The rules of real NOR flash, which everything built on top must respect:
 *   - Erase works on whole pages and sets every bit to 1 (0xFF).
 *   - Programming can only clear bits (1 -> 0). Getting a 0 back to 1 takes
 *     an erase, so programming over data that is not erased fails.
 *   - Programming works in aligned units of FLASH_WRITE_SIZE bytes.
 *   - Reads are plain memory reads: the flash is memory-mapped, and code
 *     executes in place from it.
 *   - Ranges can be write-protected (the bootloader; the running image).
 *
 * QEMU's MPS2-AN385 has no flash - its "flash" is RAM - so flash_emul.c
 * enforces these rules in software, and can inject faults (a failed program,
 * a power cut part-way through) to prove that the metadata and update logic
 * survive them. On an STM32 a short driver for the flash controller (unlock,
 * set PG/PER, write, wait for BSY to clear, check the error flags) would
 * implement the same five functions; nothing above this header changes.
 *
 * Not thread-safe: one owner per image (the bootloader; the app's OTA task).
 */
#ifndef FLASH_HAL_H
#define FLASH_HAL_H

#include <stdbool.h>
#include <stdint.h>

#define FLASH_PAGE_SIZE  4096u /* erase unit (STM32L4/G4: 2 KB; many NOR parts: 4 KB)          */
#define FLASH_WRITE_SIZE 8u    /* program unit: one 64-bit double-word, as on STM32L4/G4 (ECC) */

typedef enum {
    FLASH_OK = 0,
    FLASH_ERR_RANGE,      /* outside the flash device                              */
    FLASH_ERR_ALIGN,      /* erase not page-aligned / program not unit-aligned      */
    FLASH_ERR_PROTECTED,  /* write-protected range                                  */
    FLASH_ERR_NOT_ERASED, /* would turn a 0 bit back into 1: erase the page first   */
    FLASH_ERR_FAULT,      /* injected program failure                               */
    FLASH_ERR_POWER_LOSS  /* injected power cut: nothing changes until power-on     */
} flash_status_t;

/* Erase every page of [addr, addr + len). Both must be page-aligned. */
flash_status_t flash_erase(uint32_t addr, uint32_t len);

/* Program len bytes at addr (both aligned to FLASH_WRITE_SIZE). Checks the
 * whole range first: if any byte would need a bit set, nothing is written. */
flash_status_t flash_program(uint32_t addr, const void *data, uint32_t len);

/* Memory-mapped read access to the byte at addr (NULL if out of range). */
const uint8_t *flash_ptr(uint32_t addr);

/* Refuse every erase/program touching [addr, addr + len) from now on. On an
 * STM32 this is the write-protection (WRP) option bytes. Returns false if
 * the table of protected ranges is full. */
bool flash_protect(uint32_t addr, uint32_t len);

const char *flash_status_name(flash_status_t s);

/* ---- Emulation only: setup, statistics, fault injection ------------------ */

/* Emulate a flash of `size` bytes whose first byte has address `base`,
 * stored at host/target memory `mem` (on the target: the address itself). */
void flash_emul_init(uintptr_t mem, uint32_t base, uint32_t size);

typedef enum {
    FLASH_FAULT_NONE = 0,
    FLASH_FAULT_PROGRAM_FAIL,      /* a program fails after N more programmed bytes      */
    FLASH_FAULT_POWER_LOSS,        /* power is cut after N more programmed bytes          */
    FLASH_FAULT_POWER_LOSS_ERASE   /* power is cut after N more erased bytes              */
} flash_fault_t;

/* Arm a one-shot fault. The operation that crosses the limit does its
 * first part (up to the limit, byte-exact: a cell being programmed or
 * erased when power fails ends up half-done) and then fails. For a power
 * loss the handler runs next: on the target it resets the CPU at once, like
 * a brown-out. If it returns (unit tests), the flash stays dead - every call
 * fails with FLASH_ERR_POWER_LOSS - until flash_emul_power_on(). */
void flash_emul_inject(flash_fault_t kind, uint32_t after_bytes);
void flash_emul_set_power_loss_handler(void (*handler)(void));
void flash_emul_power_on(void);

typedef struct {
    uint32_t bytes_programmed;
    uint32_t pages_erased;
} flash_stats_t;

void flash_emul_get_stats(flash_stats_t *stats);

#endif /* FLASH_HAL_H */
