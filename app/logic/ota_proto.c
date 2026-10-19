#include "ota_proto.h"

#include <string.h>

#include "bms_msgs.h"
#include "flash_hal.h"

static uint32_t get_u24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static void put_u24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
}

static uint32_t round_up(uint32_t v, uint32_t unit)
{
    return (v + unit - 1u) / unit * unit;
}

void ota_init(ota_t *o, const ota_config_t *cfg, const ota_env_t *env, boot_meta_view_t *meta)
{
    memset(o, 0, sizeof *o);
    o->cfg = *cfg;
    o->env = *env;
    o->meta = meta;
    o->state = OTA_IDLE;
}

/* ---- responses ------------------------------------------------------------- */

static can_frame_t *respond(ota_output_t *out, ota_rsp_t type, ota_status_t status)
{
    memset(&out->response, 0, sizeof out->response);
    out->response.id = BMS_ID_OTA_RESPONSE;
    out->response.dlc = 8u;
    out->response.data[0] = (uint8_t)type;
    out->response.data[1] = (uint8_t)status;
    out->respond = true;
    out->status = status;
    return &out->response;
}

static void respond_ack(ota_output_t *out, ota_status_t status, uint32_t next_frame)
{
    put_u24(&respond(out, OTA_RSP_DATA_ACK, status)->data[2], next_frame);
}

/* Every way out of a session except success gives the lock back. */
static void end_session(ota_t *o)
{
    if (o->state == OTA_RECEIVING) {
        o->state = OTA_IDLE;
        o->env.unlock(o->env.ctx);
    }
}

static void fail(ota_t *o, ota_rsp_t type, ota_status_t status, ota_output_t *out)
{
    end_session(o);
    (void)respond(out, type, status);
    out->event = OTA_EV_FAILED;
}

static void refuse(ota_rsp_t type, ota_status_t status, ota_output_t *out)
{
    (void)respond(out, type, status);
    out->event = OTA_EV_REFUSED;
}

/* ---- Start --------------------------------------------------------------------- */

static ota_status_t check_start(ota_t *o, const can_frame_t *f, uint8_t slot, uint32_t size, uint32_t version)
{
    if (f->dlc < 8u) {
        return OTA_ERR_BAD_IMAGE; /* malformed request */
    }
    /* Only from a confirmed image with nothing staged: the inactive slot
     * holds the rollback target while a new image is on trial. */
    if (!boot_meta_update_allowed(&o->meta->meta, o->cfg.running_slot)) {
        return OTA_ERR_BUSY;
    }
    /* Execute-in-place: overwriting the running slot would pull the code out
     * from under the CPU. The flash HAL write-protects it too. */
    if (slot >= BOOT_SLOT_COUNT || slot == o->cfg.running_slot) {
        return OTA_ERR_BAD_SLOT;
    }
    if (size <= BMS_IMAGE_HEADER_SIZE || size > o->cfg.slot_size) {
        return OTA_ERR_TOO_LARGE;
    }
    /* Early refusal saves a pointless transfer; the binding check is the one
     * on the signed header at Finish (this field is not signed). */
    if (version < o->meta->meta.min_version) {
        return OTA_ERR_DOWNGRADE;
    }
    /* Last, because it has a side effect: the contactor state machine grants
     * the lock only in OPEN, and refuses CLOSE from then on. */
    if (!o->env.lock(o->env.ctx)) {
        return OTA_ERR_NOT_SAFE;
    }
    return OTA_OK;
}

static void handle_start(ota_t *o, const can_frame_t *f, uint32_t now_ms, ota_output_t *out)
{
    end_session(o); /* a new Start replaces any session in progress */

    const uint8_t slot = f->data[1];
    const uint32_t size = get_u24(&f->data[2]);
    const uint32_t version = get_u24(&f->data[5]);
    const ota_status_t st = check_start(o, f, slot, size, version);
    if (st != OTA_OK) {
        refuse(OTA_RSP_START, st, out);
        return;
    }

    /* Erase what the image will occupy before saying yes: on real flash
     * this takes a while (tens of ms per page), and the tester waits for the
     * reply before it sends data. */
    if (flash_erase(o->cfg.slot_base[slot], round_up(size, FLASH_PAGE_SIZE)) != FLASH_OK) {
        o->env.unlock(o->env.ctx);
        refuse(OTA_RSP_START, OTA_ERR_FLASH, out);
        return;
    }

    o->state = OTA_RECEIVING;
    o->target_slot = slot;
    o->image_size = size;
    o->image_version = version;
    o->next_frame = 0u;
    o->total_frames = (size + OTA_DATA_BYTES - 1u) / OTA_DATA_BYTES;
    o->started_ms = now_ms;
    o->last_rx_ms = now_ms;
    o->nack_sent = false;
    o->retransmit_requests = 0u;
    o->block_offset = 0u;
    o->block_len = 0u;
    respond(out, OTA_RSP_START, OTA_OK)->data[2] = (uint8_t)OTA_WINDOW_FRAMES;
    out->event = OTA_EV_STARTED;
}

/* ---- Data ---------------------------------------------------------------------- */

/* Program the collected window. It starts on a window boundary (a multiple
 * of 224 bytes, so of the 8-byte program unit); the tail is padded with 0xFF,
 * which programming leaves as erased. */
static ota_status_t flush_block(ota_t *o)
{
    const uint32_t len = round_up(o->block_len, FLASH_WRITE_SIZE);
    memset(&o->block[o->block_len], 0xFF, len - o->block_len);
    const uint32_t addr = o->cfg.slot_base[o->target_slot] + o->block_offset;
    if (flash_program(addr, o->block, len) != FLASH_OK) {
        return OTA_ERR_FLASH;
    }
    o->block_offset += o->block_len;
    o->block_len = 0u;
    return OTA_OK;
}

static void handle_data(ota_t *o, const can_frame_t *f, uint32_t now_ms, ota_output_t *out)
{
    if (o->state != OTA_RECEIVING) {
        return; /* stray frame, e.g. still in flight after an Abort: ignore */
    }
    o->last_rx_ms = now_ms;
    if (o->next_frame >= o->total_frames) {
        /* Everything is here, yet the tester resends: it missed the final
         * ACK. Repeat it (once per burst) instead of growing the image. */
        if (!o->nack_sent) {
            o->nack_sent = true;
            respond_ack(out, OTA_OK, o->next_frame);
        }
        return;
    }

    const uint32_t offset = o->next_frame * OTA_DATA_BYTES;
    const uint32_t remaining = o->image_size - offset;
    const uint32_t expected = (remaining < OTA_DATA_BYTES) ? remaining : OTA_DATA_BYTES;
    if (f->dlc == 0u || f->data[0] != (uint8_t)(o->next_frame & 0xFFu) || (uint32_t)(f->dlc - 1u) != expected) {
        /* Lost, repeated or malformed frame. Ask once for everything from
         * the first missing frame (go-back-N) and ignore the rest of the
         * frames already in flight until it arrives. */
        if (!o->nack_sent) {
            o->nack_sent = true;
            o->retransmit_requests++;
            respond_ack(out, OTA_ERR_SEQUENCE, o->next_frame);
            out->event = OTA_EV_RETRANSMIT;
        }
        return;
    }

    o->nack_sent = false;
    memcpy(&o->block[o->block_len], &f->data[1], expected);
    o->block_len += expected;
    o->next_frame++;

    const bool last = (o->next_frame == o->total_frames);
    if (last || (o->next_frame % OTA_WINDOW_FRAMES) == 0u) {
        const ota_status_t st = flush_block(o);
        if (st != OTA_OK) {
            fail(o, OTA_RSP_DATA_ACK, st, out);
            return;
        }
        respond_ack(out, OTA_OK, o->next_frame);
    }
}

/* ---- Finish ---------------------------------------------------------------------- */

static ota_status_t verify_image(const ota_t *o)
{
    const image_policy_t policy = {
        .slot_base = o->cfg.slot_base[o->target_slot],
        .slot_size = o->cfg.slot_size,
        .ram_start = o->cfg.ram_start,
        .ram_end = o->cfg.ram_end,
        .min_version = o->meta->meta.min_version,
        .public_key = o->cfg.public_key,
    };
    bms_image_header_t h;
    /* The same checks the bootloader will make: stage nothing it would refuse. */
    switch (bms_image_verify(flash_ptr(policy.slot_base), &policy, &h)) {
    case IMAGE_OK:
        break;
    case IMAGE_ERR_CRC:
        return OTA_ERR_CRC;
    case IMAGE_ERR_SIGNATURE:
        return OTA_ERR_SIGNATURE;
    case IMAGE_ERR_VERSION:
        return OTA_ERR_DOWNGRADE;
    default:
        return OTA_ERR_BAD_IMAGE;
    }
    /* The header must describe exactly what was announced and received. */
    return (h.image_size + BMS_IMAGE_HEADER_SIZE == o->image_size) ? OTA_OK : OTA_ERR_BAD_IMAGE;
}

static void handle_finish(ota_t *o, uint32_t now_ms, ota_output_t *out)
{
    if (o->state != OTA_RECEIVING) {
        refuse(OTA_RSP_FINISH, OTA_ERR_NO_SESSION, out);
        return;
    }
    o->last_rx_ms = now_ms;
    if (o->next_frame != o->total_frames) {
        fail(o, OTA_RSP_FINISH, OTA_ERR_INCOMPLETE, out);
        return;
    }

    ota_status_t st = verify_image(o);
    if (st == OTA_OK && !o->env.safe_to_activate(o->env.ctx)) {
        st = OTA_ERR_NOT_SAFE; /* e.g. a fault latched during the transfer */
    }
    if (st == OTA_OK) {
        boot_meta_t next = o->meta->meta;
        boot_meta_stage(&next, o->target_slot);
        if (boot_meta_save(o->cfg.store, o->meta, next) != FLASH_OK) {
            st = OTA_ERR_FLASH;
        }
    }
    if (st != OTA_OK) {
        fail(o, OTA_RSP_FINISH, st, out);
        return;
    }

    /* Success: the lock is NOT released - the contactors must stay open
     * through the reboot into the new image. */
    o->state = OTA_IDLE;
    (void)respond(out, OTA_RSP_FINISH, OTA_OK);
    out->event = OTA_EV_STAGED;
    out->reboot = true;
}

/* ---- Info ------------------------------------------------------------------------ */

static void handle_info(const ota_t *o, ota_output_t *out)
{
    const boot_meta_t *m = &o->meta->meta;
    can_frame_t *r = respond(out, OTA_RSP_INFO, OTA_OK);
    const bool confirmed = m->confirmed != 0u && m->active_slot == o->cfg.running_slot;
    r->data[2] = o->cfg.running_slot;
    r->data[3] = (uint8_t)((confirmed ? 0x01u : 0u) | ((o->state == OTA_RECEIVING) ? 0x02u : 0u) |
                           (boot_meta_update_allowed(m, o->cfg.running_slot) ? 0x04u : 0u));
    put_u24(&r->data[4], o->cfg.running_version);
    r->data[7] = (uint8_t)((o->cfg.boot_reason & 0x0Fu) | ((o->cfg.boot_attempt & 0x0Fu) << 4));
}

/* ---- dispatch ---------------------------------------------------------------- */

void ota_handle_frame(ota_t *o, const can_frame_t *f, uint32_t now_ms, ota_output_t *out)
{
    memset(out, 0, sizeof *out);
    if (f->id == BMS_ID_OTA_DATA) {
        handle_data(o, f, now_ms, out);
        return;
    }
    if (f->id != BMS_ID_OTA_REQUEST || f->dlc < 1u) {
        return;
    }
    switch ((ota_req_t)f->data[0]) {
    case OTA_REQ_START:
        handle_start(o, f, now_ms, out);
        break;
    case OTA_REQ_FINISH:
        handle_finish(o, now_ms, out);
        break;
    case OTA_REQ_ABORT:
        if (o->state == OTA_RECEIVING) {
            end_session(o);
            out->event = OTA_EV_ABORTED;
        }
        (void)respond(out, OTA_RSP_ABORT, OTA_OK);
        break;
    case OTA_REQ_INFO:
        handle_info(o, out);
        break;
    default:
        break; /* unknown request: ignore */
    }
}

void ota_poll(ota_t *o, uint32_t now_ms, ota_output_t *out)
{
    memset(out, 0, sizeof *out);
    if (o->state == OTA_RECEIVING && (uint32_t)(now_ms - o->last_rx_ms) >= o->cfg.session_timeout_ms) {
        end_session(o);
        (void)respond(out, OTA_RSP_ABORT, OTA_ERR_TIMEOUT);
        out->event = OTA_EV_TIMEOUT;
    }
}

const char *ota_status_name(ota_status_t s)
{
    switch (s) {
    case OTA_OK:             return "ok";
    case OTA_ERR_NOT_SAFE:   return "contactors not open";
    case OTA_ERR_BUSY:       return "running image not confirmed yet, or an update already staged";
    case OTA_ERR_BAD_SLOT:   return "target is not the inactive slot";
    case OTA_ERR_TOO_LARGE:  return "image does not fit the slot";
    case OTA_ERR_DOWNGRADE:  return "version below the anti-rollback minimum";
    case OTA_ERR_SEQUENCE:   return "frame out of sequence";
    case OTA_ERR_CRC:        return "CRC mismatch (corrupted image)";
    case OTA_ERR_SIGNATURE:  return "bad signature";
    case OTA_ERR_BAD_IMAGE:  return "not a valid image for this slot";
    case OTA_ERR_FLASH:      return "flash write failed";
    case OTA_ERR_NO_SESSION: return "no update in progress";
    case OTA_ERR_TIMEOUT:    return "timed out";
    case OTA_ERR_INCOMPLETE: return "image incomplete";
    default:                 return "?";
    }
}
