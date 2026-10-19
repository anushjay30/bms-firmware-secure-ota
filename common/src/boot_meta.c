#include "boot_meta.h"

#include <stddef.h>
#include <string.h>

#include "crc32.h"

#define RECORD_CRC_BYTES ((uint32_t)offsetof(boot_meta_t, crc32))

void boot_meta_defaults(boot_meta_t *m)
{
    memset(m, 0, sizeof *m);
    m->magic = BOOT_META_MAGIC;
    m->layout = BOOT_META_LAYOUT;
    m->size = (uint16_t)sizeof *m;
    m->active_slot = BOOT_SLOT_A;
    m->pending_slot = BOOT_SLOT_NONE;
    m->confirmed = 1u;
}

static bool record_valid(const boot_meta_t *m)
{
    return m->magic == BOOT_META_MAGIC && m->layout == BOOT_META_LAYOUT && m->size == sizeof *m &&
           m->crc32 == crc32(m, RECORD_CRC_BYTES) &&
           /* a record with a valid CRC but impossible contents is still rejected */
           m->active_slot < BOOT_SLOT_COUNT &&
           (m->pending_slot < BOOT_SLOT_COUNT || m->pending_slot == BOOT_SLOT_NONE) && m->confirmed <= 1u;
}

/* "a is newer than b", correct across the 32-bit wrap (it never happens in
 * practice - a few writes per update - but costs nothing to get right). */
static bool seq_newer(uint32_t a, uint32_t b)
{
    const uint32_t d = a - b;
    return d != 0u && d < 0x80000000u;
}

void boot_meta_load(const boot_meta_store_t *store, boot_meta_view_t *view)
{
    boot_meta_t rec[2];
    for (unsigned i = 0; i < 2u; i++) {
        const uint8_t *p = flash_ptr(store->copy_addr[i]);
        view->valid[i] = false;
        if (p != NULL) {
            memcpy(&rec[i], p, sizeof rec[i]);
            view->valid[i] = record_valid(&rec[i]);
        }
    }

    if (view->valid[0] && view->valid[1]) {
        view->copy = seq_newer(rec[1].sequence, rec[0].sequence) ? 1 : 0;
    } else if (view->valid[0]) {
        view->copy = 0;
    } else if (view->valid[1]) {
        view->copy = 1;
    } else {
        view->copy = -1;
    }

    if (view->copy >= 0) {
        view->meta = rec[view->copy];
    } else {
        boot_meta_defaults(&view->meta);
    }
}

flash_status_t boot_meta_save(const boot_meta_store_t *store, boot_meta_view_t *view, boot_meta_t next)
{
    /* Never touch the copy the current state came from: it is the one we
     * fall back to if this write is interrupted. */
    const int target = (view->copy == 0) ? 1 : 0;
    const uint32_t addr = store->copy_addr[target];

    next.magic = BOOT_META_MAGIC;
    next.layout = BOOT_META_LAYOUT;
    next.size = (uint16_t)sizeof next;
    next.sequence = (view->copy >= 0) ? view->meta.sequence + 1u : 1u;
    next.reserved[0] = 0u;
    next.reserved[1] = 0u;
    next.crc32 = crc32(&next, RECORD_CRC_BYTES); /* the CRC is the last field programmed: the commit point */

    view->valid[target] = false; /* from the first erased byte on, this copy is garbage */
    flash_status_t st = flash_erase(addr, FLASH_PAGE_SIZE);
    if (st == FLASH_OK) {
        st = flash_program(addr, &next, (uint32_t)sizeof next);
    }
    if (st == FLASH_OK) {
        view->meta = next;
        view->copy = target;
        view->valid[target] = true;
    }
    return st;
}

/* ---- Bootloader decision --------------------------------------------------- */

typedef struct {
    boot_check_fn     fn;
    void             *ctx;
    uint32_t          min_version;
    bool              done[BOOT_SLOT_COUNT];
    boot_slot_check_t result[BOOT_SLOT_COUNT];
} checker_t;

/* Each slot is verified at most once per boot: the signature check is the
 * slowest part of booting. */
static boot_slot_check_t check_slot(checker_t *c, uint8_t slot)
{
    if (!c->done[slot]) {
        c->result[slot] = c->fn(slot, c->min_version, c->ctx);
        c->done[slot] = true;
    }
    return c->result[slot];
}

boot_decision_t boot_decide(const boot_meta_t *cur, boot_check_fn check, void *ctx, boot_meta_t *next)
{
    boot_decision_t d = { .slot = BOOT_SLOT_NONE, .version = 0u, .reason = BOOT_REASON_NORMAL, .attempt = 0u,
                          .other_slot = BOOT_SLOT_NONE, .meta_changed = false, .note = "confirmed image" };
    checker_t chk = { .fn = check, .ctx = ctx, .min_version = cur->min_version, .done = { false, false } };
    *next = *cur;

    /* 1. An update the application staged becomes the trial image - if it
     *    verifies. (The app checked it too; we check again because only what
     *    the bootloader verifies itself is trusted.) */
    if (next->pending_slot != BOOT_SLOT_NONE) {
        const uint8_t staged = next->pending_slot;
        next->pending_slot = BOOT_SLOT_NONE;
        d.meta_changed = true;
        if (staged != next->active_slot && check_slot(&chk, staged).valid) {
            next->active_slot = staged;
            next->confirmed = 0u;
            next->boot_attempts = 0u;
        } else {
            d.other_slot = staged;
            d.note = "staged image failed verification: ignored";
        }
    }

    /* 2. A trial image gets BOOT_MAX_TRIAL_BOOTS boots to confirm itself. The
     *    counter is persisted before the image runs, so one that crashes or
     *    hangs until a watchdog/power-cycle reset still uses up its attempts. */
    if (next->confirmed == 0u) {
        next->boot_attempts++;
        d.meta_changed = true;
        if (next->boot_attempts > BOOT_MAX_TRIAL_BOOTS) {
            d.reason = BOOT_REASON_ROLLBACK;
            d.other_slot = next->active_slot;
            d.note = "trial image never confirmed itself: rolling back";
            next->active_slot = boot_other_slot(next->active_slot);
            next->confirmed = 1u;
            next->boot_attempts = 0u;
        } else {
            d.reason = BOOT_REASON_TRIAL;
            d.attempt = next->boot_attempts;
            d.note = "trial boot of a new image";
        }
    }

    /* 3. Start the chosen slot only if its image is valid; otherwise the other
     *    slot, if that one is. */
    boot_slot_check_t c = check_slot(&chk, next->active_slot);
    if (!c.valid) {
        const uint8_t bad = next->active_slot;
        const boot_slot_check_t alt = check_slot(&chk, boot_other_slot(bad));
        if (!alt.valid) {
            d.slot = BOOT_SLOT_NONE;
            d.reason = BOOT_REASON_NONE;
            d.note = "no valid image in either slot";
            return d;
        }
        d.note = (d.reason == BOOT_REASON_TRIAL) ? "trial image no longer valid: rolling back"
                                                 : "active image invalid: starting the other slot";
        d.reason = (d.reason == BOOT_REASON_TRIAL) ? BOOT_REASON_ROLLBACK : BOOT_REASON_FALLBACK;
        d.attempt = 0u;
        d.other_slot = bad;
        next->active_slot = boot_other_slot(bad);
        next->confirmed = 1u;
        next->boot_attempts = 0u;
        d.meta_changed = true;
        c = alt;
    }

    /* 4. The anti-rollback floor follows the confirmed image (this also
     *    records the factory image's version on the very first boot). */
    if (next->confirmed != 0u && c.version > next->min_version) {
        next->min_version = c.version;
        d.meta_changed = true;
    }
    d.slot = next->active_slot;
    d.version = c.version;
    return d;
}

const char *boot_reason_name(boot_reason_t r)
{
    switch (r) {
    case BOOT_REASON_NORMAL:   return "normal";
    case BOOT_REASON_TRIAL:    return "trial";
    case BOOT_REASON_ROLLBACK: return "rollback";
    case BOOT_REASON_FALLBACK: return "fallback";
    case BOOT_REASON_NONE:     return "none";
    default:                   return "?";
    }
}

/* ---- Application side ------------------------------------------------------ */

bool boot_meta_update_allowed(const boot_meta_t *m, uint8_t running_slot)
{
    return m->confirmed == 1u && m->pending_slot == BOOT_SLOT_NONE && m->active_slot == running_slot;
}

void boot_meta_stage(boot_meta_t *m, uint8_t slot)
{
    m->pending_slot = slot;
}

void boot_meta_confirm(boot_meta_t *m, uint32_t version)
{
    m->confirmed = 1u;
    m->boot_attempts = 0u;
    if (version > m->min_version) {
        m->min_version = version;
    }
}
