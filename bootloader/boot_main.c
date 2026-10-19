/*
 * boot_main.c - Boot flow of the BMS bootloader.
 *
 *   1. read the boot metadata (newest valid of two copies)
 *   2. decide which slot to start (staged update -> trial boot, trial
 *      attempts, rollback, fallback; common/src/boot_meta.c), verifying each
 *      candidate image: header, anti-rollback floor, CRC-32, Ed25519 signature
 *   3. persist the new metadata BEFORE starting anything, so a trial image
 *      that crashes has still used up one of its attempts
 *   4. tell the application what happened (SHARED RAM mailbox), jump
 *
 * If neither slot holds a valid image it stays here and says so on UART0.
 */
#include <stddef.h>
#include <stdint.h>

#include "bms_image.h"
#include "bms_keys.h"
#include "boot_info.h"
#include "boot_jump.h"
#include "boot_log.h"
#include "boot_meta.h"
#include "crc32.h"
#include "flash_hal.h"
#include "memory_map.h"
#include "mps2_an385.h"

_Static_assert(MM_META_COPY1 - MM_META_COPY0 >= FLASH_PAGE_SIZE, "metadata copies must not share an erase page");
_Static_assert(MM_META_COPY1 + FLASH_PAGE_SIZE <= MM_META_BASE + MM_META_SIZE, "metadata copies must fit in META");

void boot_main(void) __attribute__((noreturn));

static const boot_meta_store_t s_store = { { MM_META_COPY0, MM_META_COPY1 } };

static uint32_t slot_base(uint8_t slot)
{
    return (slot == BOOT_SLOT_A) ? MM_SLOT_A_BASE : MM_SLOT_B_BASE;
}

/* Verification time, measured with SysTick free-running on the 25 MHz
 * processor clock (polled, no interrupt; good for up to 0.67 s). Under
 * QEMU's "-icount shift=0" every instruction takes exactly 1 ns of guest
 * time, so the same number reads as an instruction count: a cost that does
 * not depend on the host. (boot_jump stops SysTick again before the app.) */
static void timer_start(void)
{
    SYST_RVR = 0x00FFFFFFu;
    SYST_CVR = 0u;
    SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_ENABLE;
}

static uint32_t timer_elapsed_us(void)
{
    const uint32_t counts = (0x00FFFFFFu - SYST_CVR) & 0x00FFFFFFu;
    return counts / (MPS2_SYSCLK_HZ / 1000000u);
}

/* Full verification of one slot against the current anti-rollback floor;
 * boot_decide() calls this for each slot it considers. */
static boot_slot_check_t check_slot(uint8_t slot, uint32_t min_version, void *ctx)
{
    (void)ctx;
    const image_policy_t policy = {
        .slot_base = slot_base(slot),
        .slot_size = MM_SLOT_SIZE,
        .ram_start = MM_RAM_BASE,
        .ram_end = MM_RAM_BASE + MM_RAM_SIZE,
        .min_version = min_version,
        .public_key = g_bms_signing_pubkey,
    };
    bms_image_header_t h;
    timer_start();
    const image_result_t r = bms_image_verify(flash_ptr(policy.slot_base), &policy, &h);
    const uint32_t us = timer_elapsed_us();
    if (r == IMAGE_OK) {
        boot_printf("BOOT  slot %c: v%v, %u bytes, CRC and signature ok (%u us)\n", (uint32_t)boot_slot_name(slot),
                    h.image_version, h.image_size, us);
    } else if (r == IMAGE_ERR_VERSION) {
        boot_printf("BOOT  slot %c: rejected, version below the minimum v%v (anti-rollback)\n",
                    (uint32_t)boot_slot_name(slot), min_version);
    } else {
        boot_printf("BOOT  slot %c: %s\n", (uint32_t)boot_slot_name(slot), bms_image_result_name(r));
    }
    const boot_slot_check_t c = { .valid = (r == IMAGE_OK), .version = (r == IMAGE_OK) ? h.image_version : 0u };
    return c;
}

static void log_meta(const boot_meta_view_t *v)
{
    const boot_meta_t *m = &v->meta;
    if (v->copy < 0) {
        boot_printf("BOOT  metadata: no valid copy, factory defaults\n");
    } else {
        boot_printf("BOOT  metadata: copy %u (seq %u)%s\n", (uint32_t)v->copy, m->sequence,
                    v->valid[v->copy == 0 ? 1 : 0] ? "" : ", other copy invalid");
    }
    boot_printf("BOOT  active %c (%s, %u trial boots), pending %c, minimum version v%v\n",
                (uint32_t)boot_slot_name(m->active_slot), m->confirmed ? "confirmed" : "on trial",
                (uint32_t)m->boot_attempts, (uint32_t)boot_slot_name(m->pending_slot), m->min_version);
}

static void publish_boot_info(const boot_decision_t *d, uint32_t version, uint32_t min_version)
{
    boot_info_t info = {
        .magic = BOOT_INFO_MAGIC,
        .slot = d->slot,
        .reason = (uint8_t)d->reason,
        .attempt = d->attempt,
        .other_slot = d->other_slot,
        .image_version = version,
        .min_version = min_version,
        .crc32 = 0u,
    };
    info.crc32 = crc32(&info, offsetof(boot_info_t, crc32));
    *(volatile boot_info_t *)MM_SHARED_BASE = info;
}

void boot_main(void)
{
    boot_log_init();
    boot_printf("\nBOOT  BMS bootloader (A/B, Ed25519 key %s)\n", g_bms_signing_key_id);

    /* SSRAM1 plays flash; the bootloader region is never written - neither by
     * us nor (once the app sets the same protection) by anyone else. */
    flash_emul_init(0u, MM_FLASH_BASE, MM_FLASH_SIZE);
    (void)flash_protect(MM_BOOT_BASE, MM_BOOT_SIZE);

    boot_meta_view_t view;
    boot_meta_load(&s_store, &view);
    log_meta(&view);

    boot_meta_t next;
    boot_decision_t d = boot_decide(&view.meta, check_slot, NULL, &next);

    if (d.meta_changed) {
        const flash_status_t st = boot_meta_save(&s_store, &view, next);
        if (st != FLASH_OK) {
            boot_printf("BOOT  metadata write failed: %s\n", flash_status_name(st));
            if (d.reason == BOOT_REASON_TRIAL) {
                /* The attempt counter could not be persisted: a trial image
                 * that crashes would get unlimited boots. Start the previous
                 * image instead and retry the trial next time. */
                const uint8_t prev = boot_other_slot(d.slot);
                const boot_slot_check_t c = check_slot(prev, view.meta.min_version, NULL);
                if (c.valid) {
                    d.slot = prev;
                    d.version = c.version;
                    d.reason = BOOT_REASON_NORMAL;
                    d.attempt = 0u;
                    d.note = "metadata not writable: trial postponed";
                }
            }
        }
    }

    if (d.slot == BOOT_SLOT_NONE) {
        boot_printf("BOOT  NO VALID IMAGE - staying in the bootloader (%s)\n", d.note);
        for (;;) {
            __asm volatile("wfi");
        }
    }

    publish_boot_info(&d, d.version, view.meta.min_version);
    if (d.reason == BOOT_REASON_TRIAL) {
        boot_printf("BOOT  starting slot %c: %s %u of %u\n", (uint32_t)boot_slot_name(d.slot), d.note,
                    (uint32_t)d.attempt, (uint32_t)BOOT_MAX_TRIAL_BOOTS);
    } else {
        boot_printf("BOOT  starting slot %c: %s\n", (uint32_t)boot_slot_name(d.slot), d.note);
    }
    /* The verified header said vector_table == slot + 0x100; nothing else
     * is read from the image after verification. */
    boot_jump_to_app(slot_base(d.slot) + MM_IMAGE_HEADER_SIZE);
}
