/*
 * task_ota.c - Firmware update service, and the only owner of the flash.
 *
 *  - At startup it reads the boot metadata and what the bootloader reported,
 *    and works out whether this image is on trial.
 *  - When the supervisor judges the system healthy (health.c), it confirms
 *    the image: the bootloader will no longer roll it back.
 *  - It receives updates over CAN (ota_proto.c) into the inactive slot,
 *    verifies and stages them, and reboots into them.
 *
 * One task owns the flash HAL and the metadata, so there is never a second
 * writer and no lock is needed. It runs at low priority (BMS_PRIO_OTA): flash
 * writes and signature checks take time, and none of that may delay the
 * monitoring and safety tasks - cells are monitored throughout a transfer.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "bms_port_config.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "bms_hal.h"
#include "bms_log.h"
#include "bms_tasks.h"

#if BMS_PORT_HAS_OTA

#include "boot_meta.h"
#include "flash_hal.h"
#include "ota_port.h"
#include "ota_proto.h"

typedef enum {
    OTA_MSG_FRAME,   /* a CAN frame on one of the OTA identifiers        */
    OTA_MSG_CONFIRM, /* the supervisor says: healthy for long enough     */
    OTA_MSG_INJECT   /* test hook: arm a flash fault                     */
} ota_msg_type_t;

typedef struct {
    ota_msg_type_t type;
    can_frame_t    frame;      /* OTA_MSG_FRAME  */
    bool           power_loss; /* OTA_MSG_INJECT */
    uint32_t       bytes;      /* OTA_MSG_INJECT */
} ota_msg_t;

#define VERSION_TEXT_LEN 16u /* "65535.255.255" */

/* Deep enough for a whole transfer window plus a few commands: the window
 * is the protocol's flow control, so a well-behaved tester cannot overflow it. */
#define OTA_QUEUE_LEN (OTA_WINDOW_FRAMES + 8u)

static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[OTA_QUEUE_LEN * sizeof(ota_msg_t)];
static QueueHandle_t s_queue;

static volatile bool s_on_trial; /* read by the supervisor */
static boot_meta_view_t s_meta;  /* current metadata, kept in sync with flash */
static ota_t s_ota;

static void version_text(uint32_t v, char out[VERSION_TEXT_LEN])
{
    (void)snprintf(out, VERSION_TEXT_LEN, "%lu.%lu.%lu", (unsigned long)(v >> 16),
                   (unsigned long)((v >> 8) & 0xFFu), (unsigned long)(v & 0xFFu));
}

/* ---- contactor interlock (ota_env_t) -------------------------------------- */

static bool env_lock(void *ctx)
{
    (void)ctx;
    return shared_hv_lock_acquire(200u);
}

static void env_unlock(void *ctx)
{
    (void)ctx;
    shared_hv_lock_release();
}

/* Activation needs what Start needed: contactors open - and no fault latched
 * during the transfer (a reboot would silently clear it). */
static bool env_safe(void *ctx)
{
    (void)ctx;
    bms_status_msg_t st;
    shared_get_status(&st);
    return st.state == BMS_STATE_OPEN && st.faults_latched == 0u;
}

/* ---- startup ---------------------------------------------------------------- */

static void log_boot_report(const boot_info_t *bi, bool valid)
{
    char v[VERSION_TEXT_LEN];
    version_text(ota_port_running_version(), v);
    if (!valid) {
        bms_log("OTA", "v%s in slot %c: no report from the bootloader (started by a debugger?)", v,
                boot_slot_name(ota_port_running_slot()));
    } else if (bi->reason == BOOT_REASON_TRIAL) {
        bms_log("OTA", "v%s in slot %c: trial boot %u of %u, not confirmed yet", v, boot_slot_name(bi->slot),
                (unsigned)bi->attempt, (unsigned)BOOT_MAX_TRIAL_BOOTS);
    } else if (bi->reason == BOOT_REASON_ROLLBACK || bi->reason == BOOT_REASON_FALLBACK) {
        bms_log("OTA", "v%s in slot %c: %s - slot %c was given up", v, boot_slot_name(bi->slot),
                boot_reason_name((boot_reason_t)bi->reason), boot_slot_name(bi->other_slot));
    } else {
        bms_log("OTA", "v%s in slot %c: confirmed image", v, boot_slot_name(bi->slot));
    }
}

static void init_protocol(const boot_info_t *bi, bool bi_valid)
{
    const image_policy_t any = ota_port_image_policy(BOOT_SLOT_A, 0u); /* RAM range, key */
    const ota_config_t cfg = {
        .running_slot = ota_port_running_slot(),
        .running_version = ota_port_running_version(),
        .slot_base = { ota_port_slot_base(BOOT_SLOT_A), ota_port_slot_base(BOOT_SLOT_B) },
        .slot_size = ota_port_slot_size(),
        .ram_start = any.ram_start,
        .ram_end = any.ram_end,
        .public_key = any.public_key,
        .store = ota_port_meta_store(),
        .session_timeout_ms = BMS_OTA_SESSION_TIMEOUT_MS,
        .boot_reason = bi_valid ? bi->reason : (uint8_t)BOOT_REASON_NONE,
        .boot_attempt = bi_valid ? bi->attempt : 0u,
    };
    const ota_env_t env = { .lock = env_lock, .unlock = env_unlock, .safe_to_activate = env_safe, .ctx = NULL };
    ota_init(&s_ota, &cfg, &env, &s_meta);
}

/* ---- confirm ------------------------------------------------------------------ */

static void confirm_image(void)
{
    if (!s_on_trial) {
        return;
    }
    char v[VERSION_TEXT_LEN];
    const uint32_t version = ota_port_running_version();
    version_text(version, v);
    boot_meta_t next = s_meta.meta;
    boot_meta_confirm(&next, version);
    const flash_status_t st = boot_meta_save(ota_port_meta_store(), &s_meta, next);
    if (st != FLASH_OK) {
        /* The supervisor asks again after another healthy period. */
        bms_log("OTA", "confirming v%s failed: %s", v, flash_status_name(st));
        return;
    }
    s_on_trial = false;
    bms_log("OTA", "image v%s confirmed after %u ms of healthy operation; minimum version is now v%s", v,
            (unsigned)BMS_CONFIRM_HEALTHY_MS, v);
}

/* ---- protocol output ------------------------------------------------------ */

static void send_response(const can_frame_t *f)
{
    /* The TX queue is shared with the cyclic status; if it is momentarily
     * full, wait a tick rather than lose a reply the tester waits for. */
    for (unsigned attempt = 0; attempt < 10u; attempt++) {
        if (bms_request_frame(f)) {
            return;
        }
        vTaskDelay(1);
    }
    bms_log("OTA", "reply dropped: CAN TX queue full");
}

static void log_event(const ota_output_t *out)
{
    char v[VERSION_TEXT_LEN];
    const char slot = boot_slot_name(s_ota.target_slot);
    switch (out->event) {
    case OTA_EV_STARTED:
        version_text(s_ota.image_version, v);
        bms_log("OTA", "update started: v%s, %lu bytes for slot %c", v, (unsigned long)s_ota.image_size, slot);
        break;
    case OTA_EV_REFUSED:
        bms_log("OTA", "update request refused: %s", ota_status_name(out->status));
        break;
    case OTA_EV_STAGED: {
        uint32_t staged; /* the verified header's version, not the announced one */
        memcpy(&staged, flash_ptr(ota_port_slot_base(s_ota.target_slot)) + offsetof(bms_image_header_t, image_version),
               sizeof staged);
        version_text(staged, v);
        bms_log("OTA", "v%s verified (CRC, Ed25519, version), staged for slot %c: %lu B in %lu ms, %lu resend(s)", v,
                slot, (unsigned long)s_ota.image_size, (unsigned long)(bms_now_ms() - s_ota.started_ms),
                (unsigned long)s_ota.retransmit_requests);
        break;
    }
    case OTA_EV_FAILED:
        bms_log("OTA", "update failed: %s - slot %c will not be used", ota_status_name(out->status), slot);
        break;
    case OTA_EV_ABORTED:
        bms_log("OTA", "update aborted by the tester");
        break;
    case OTA_EV_TIMEOUT:
        bms_log("OTA", "update abandoned: no frame for %u ms", (unsigned)BMS_OTA_SESSION_TIMEOUT_MS);
        break;
    case OTA_EV_RETRANSMIT: /* counted, reported with the result */
    case OTA_EV_NONE:
    default:
        break;
    }
}

static void reboot_into_new_image(void)
{
    bms_log("OTA", "rebooting into the new image");
    /* Let the Finish reply leave on the CAN link and the log drain: the
     * reset loses whatever is still queued. */
    vTaskDelay(pdMS_TO_TICKS(100));
    bms_log_flush(500u);

    /* We hold the update lock, so the contactors cannot have closed - but
     * a reset with HV connected is exactly what must never happen, so check
     * rather than trust. (OPEN and FAULT both mean every relay is open.) */
    bms_status_msg_t st;
    shared_get_status(&st);
    if (st.state != BMS_STATE_OPEN && st.state != BMS_STATE_FAULT) {
        bms_log("OTA", "NOT rebooting: contactors are %s; the new image starts at the next reset",
                bms_state_name(st.state));
        shared_hv_lock_release();
        return;
    }
    hal_system_reset();
}

static void handle_output(const ota_output_t *out)
{
    if (out->respond) {
        send_response(&out->response);
    }
    log_event(out);
    if (out->reboot) {
        reboot_into_new_image();
    }
}

static void inject_fault(bool power_loss, uint32_t bytes)
{
    flash_emul_inject(power_loss ? FLASH_FAULT_POWER_LOSS : FLASH_FAULT_PROGRAM_FAIL, bytes);
    bms_log("TEST", "flash fault armed: %s after %lu more programmed bytes",
            power_loss ? "power loss" : "write failure", (unsigned long)bytes);
}

/* ---- task ------------------------------------------------------------------------ */

void bms_task_ota(void *arg)
{
    (void)arg;
    ota_port_flash_init();
    boot_meta_load(ota_port_meta_store(), &s_meta);
    boot_info_t bi;
    const bool bi_valid = ota_port_boot_info(&bi);
    log_boot_report(&bi, bi_valid);
    s_on_trial = s_meta.meta.active_slot == ota_port_running_slot() && s_meta.meta.confirmed == 0u;
#if defined(BMS_TEST_NO_CONFIRM)
    bms_log("TEST", "test build: this image never confirms itself");
#endif
    init_protocol(&bi, bi_valid);
    bms_task_watch(BMS_TASK_OTA, BMS_OTA_WDG_TIMEOUT_MS);

    for (;;) {
        bms_task_checkin(BMS_TASK_OTA);
        bms_test_hook_poll(BMS_TASK_OTA);

        ota_output_t out;
        ota_msg_t msg;
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(BMS_MONITOR_PERIOD_MS)) == pdPASS) {
            switch (msg.type) {
            case OTA_MSG_FRAME:
                ota_handle_frame(&s_ota, &msg.frame, bms_now_ms(), &out);
                handle_output(&out);
                break;
            case OTA_MSG_CONFIRM:
                confirm_image();
                break;
            case OTA_MSG_INJECT:
                inject_fault(msg.power_loss, msg.bytes);
                break;
            default:
                break;
            }
        }
        ota_poll(&s_ota, bms_now_ms(), &out); /* session timeout */
        handle_output(&out);
    }
}

void bms_ota_init(void)
{
    s_queue = xQueueCreateStatic(OTA_QUEUE_LEN, sizeof(ota_msg_t), s_queue_storage, &s_queue_struct);
    configASSERT(s_queue != NULL);
}

void bms_ota_post_frame(const can_frame_t *f)
{
    const ota_msg_t msg = { .type = OTA_MSG_FRAME, .frame = *f };
    (void)xQueueSend(s_queue, &msg, 0); /* full: dropped; the protocol retransmits */
}

bool bms_ota_needs_confirm(void)
{
#if defined(BMS_TEST_NO_CONFIRM)
    return false;
#else
    return s_on_trial;
#endif
}

void bms_ota_request_confirm(void)
{
    const ota_msg_t msg = { .type = OTA_MSG_CONFIRM };
    (void)xQueueSend(s_queue, &msg, 0);
}

void bms_ota_inject_flash_fault(bool power_loss, uint32_t bytes)
{
    const ota_msg_t msg = { .type = OTA_MSG_INJECT, .power_loss = power_loss, .bytes = bytes };
    (void)xQueueSend(s_queue, &msg, 0);
}

#else /* !BMS_PORT_HAS_OTA: no bootloader, nothing to update or confirm */

void bms_ota_init(void) {}

void bms_ota_post_frame(const can_frame_t *f)
{
    (void)f;
}

bool bms_ota_needs_confirm(void)
{
    return false;
}

void bms_ota_request_confirm(void) {}

void bms_ota_inject_flash_fault(bool power_loss, uint32_t bytes)
{
    (void)power_loss;
    (void)bytes;
}

#endif
