/*
 * boot_meta.h - Boot metadata: which slot to start, the trial-boot state of
 * a new image, and the anti-rollback floor. Shared by the bootloader (which
 * decides) and the application (which stages updates and confirms itself).
 *
 * Power-loss safety: two copies, each a 32-byte record with a sequence number
 * and a CRC, in two separate flash pages. Reading uses the valid copy with
 * the highest sequence number. Writing always goes to the OTHER copy (the
 * older or invalid one), with sequence + 1: erase its page, program the
 * record. A power cut at any instant can only damage the copy being written,
 * whose CRC then fails, and the next read falls back to the untouched copy -
 * the state before the write. Once the record's last bytes are programmed,
 * the new copy is valid with the highest sequence number - the state after
 * the write. There is no instant at which the newest valid state is lost, and
 * no instant at which a half-written state can be read as valid.
 *
 * Fields and their lifecycle:
 *   active_slot    the slot the bootloader starts
 *   pending_slot   set by the app when it has written and verified an update
 *                  into the other slot; the bootloader consumes it and makes
 *                  that slot active on trial
 *   confirmed      0 while the active image is on trial; set by the app once
 *                  its health check passes
 *   boot_attempts  trial boots so far; past BOOT_MAX_TRIAL_BOOTS without a
 *                  confirmation the bootloader rolls back to the other slot
 *   min_version    anti-rollback floor: no image older than this boots or is
 *                  accepted as an update. Raised when an image is confirmed.
 */
#ifndef BOOT_META_H
#define BOOT_META_H

#include <stdbool.h>
#include <stdint.h>

#include "flash_hal.h"

#define BOOT_SLOT_A           0u
#define BOOT_SLOT_B           1u
#define BOOT_SLOT_NONE        0xFFu
#define BOOT_SLOT_COUNT       2u

/* A new image gets this many boots to confirm itself. */
#define BOOT_MAX_TRIAL_BOOTS  3u

#define BOOT_META_MAGIC       0x4D534D42u /* bytes 'B','M','S','M' */
#define BOOT_META_LAYOUT      1u

typedef struct {
    uint32_t magic;
    uint16_t layout;
    uint16_t size;
    uint32_t sequence;
    uint8_t  active_slot;
    uint8_t  pending_slot;
    uint8_t  confirmed;
    uint8_t  boot_attempts;
    uint32_t min_version;
    uint32_t reserved[2];
    uint32_t crc32;        /* CRC-32 of every byte above; written last */
} boot_meta_t;

_Static_assert(sizeof(boot_meta_t) == 32u, "metadata record is 32 bytes");
_Static_assert(sizeof(boot_meta_t) % FLASH_WRITE_SIZE == 0u, "record must be whole program units");

/* Where the two copies live: page-aligned flash addresses, separate pages. */
typedef struct {
    uint32_t copy_addr[2];
} boot_meta_store_t;

/* Result of reading both copies. */
typedef struct {
    boot_meta_t meta;     /* newest valid record, or the factory defaults     */
    int         copy;     /* index meta came from; -1 if no copy was valid    */
    bool        valid[2]; /* per copy (diagnostics)                           */
} boot_meta_view_t;

/* Factory state, used when no copy is valid (blank META): slot A, confirmed,
 * no anti-rollback floor yet. */
void boot_meta_defaults(boot_meta_t *m);

void boot_meta_load(const boot_meta_store_t *store, boot_meta_view_t *view);

/* Write `next` as the new newest copy (sequence and CRC are filled in) into
 * the copy that `view` did not come from, and update `view` to match. */
flash_status_t boot_meta_save(const boot_meta_store_t *store, boot_meta_view_t *view, boot_meta_t next);

/* ---- Bootloader: what to start ------------------------------------------- */

typedef enum {
    BOOT_REASON_NORMAL = 0, /* the confirmed active image                         */
    BOOT_REASON_TRIAL,      /* a new image, not yet confirmed                     */
    BOOT_REASON_ROLLBACK,   /* a trial image failed: back to the previous one     */
    BOOT_REASON_FALLBACK,   /* the active image is broken: the other slot instead */
    BOOT_REASON_NONE        /* nothing bootable                                   */
} boot_reason_t;

/* Verdict on one slot, supplied by the caller (the bootloader runs the full
 * image check against the current anti-rollback floor). */
typedef struct {
    bool     valid;
    uint32_t version; /* when valid */
} boot_slot_check_t;

typedef boot_slot_check_t (*boot_check_fn)(uint8_t slot, uint32_t min_version, void *ctx);

typedef struct {
    uint8_t       slot;          /* slot to start, BOOT_SLOT_NONE if none         */
    uint32_t      version;       /* version of the image in that slot             */
    boot_reason_t reason;
    uint8_t       attempt;       /* trial boot number when reason == TRIAL        */
    uint8_t       other_slot;    /* slot given up (rollback, fallback, ignored)   */
    bool          meta_changed;  /* *next differs: persist it before starting     */
    const char   *note;          /* static text for the log                       */
} boot_decision_t;

/* The A/B state machine. Pure: all flash and crypto work happens in check(). */
boot_decision_t boot_decide(const boot_meta_t *cur, boot_check_fn check, void *ctx, boot_meta_t *next);

const char *boot_reason_name(boot_reason_t r);

/* ---- Application: stage, confirm --------------------------------------------- */

/* An update may only start when the running image is confirmed, nothing is
 * staged, and the metadata agrees about which slot is running: the other
 * slot holds the image a failed update would roll back to, so it must not be
 * overwritten while that could still happen. */
bool boot_meta_update_allowed(const boot_meta_t *m, uint8_t running_slot);

/* A verified image in `slot` becomes the trial image at the next boot. */
void boot_meta_stage(boot_meta_t *m, uint8_t slot);

/* The running (active) image passed its health check: make it permanent and
 * raise the anti-rollback floor to its version. */
void boot_meta_confirm(boot_meta_t *m, uint32_t version);

static inline uint8_t boot_other_slot(uint8_t slot)
{
    return (slot == BOOT_SLOT_A) ? BOOT_SLOT_B : BOOT_SLOT_A;
}

static inline char boot_slot_name(uint8_t slot)
{
    return (slot == BOOT_SLOT_A) ? 'A' : (slot == BOOT_SLOT_B) ? 'B' : '-';
}

#endif /* BOOT_META_H */
