/*
 * ota_proto.h - Firmware update over CAN: the receiving side (pure logic).
 *
 *   tester                                    BMS
 *   Start(slot, size, version)  -------->   checks; takes the update lock
 *                               <--------   Start Ok (window W)  | error status
 *   Data #0 .. #W-1             -------->   programmed into the inactive slot
 *                               <--------   DataAck(Ok, next = W)
 *     ...                                   (lost/duplicate/bad frame:
 *                               <--------    DataAck(Sequence, next = n): resend from n)
 *   Finish                      -------->   verify CRC + Ed25519 + version; stage
 *                               <--------   Finish Ok, then reboot  | error status
 *   Abort / GetInfo             -------->   end the session / report state
 *
 * Data frames carry a sequence byte (frame index mod 256) and 7 image bytes;
 * the BMS answers every window of OTA_WINDOW_FRAMES with one cumulative ACK
 * - flow control: the tester never has more than one window in flight, which
 * the update task's queue is sized for - and asks for a retransmission from
 * the first missing frame on any gap (go-back-N). The tester retransmits from
 * the last ACK if no answer comes; the BMS abandons a session that goes
 * quiet for BMS_OTA_SESSION_TIMEOUT_MS.
 *
 * Safety (the contactors): Start takes the update lock from the contactor
 * state machine, which grants it only while the contactors are OPEN and then
 * refuses CLOSE until the session ends. The update never opens anything
 * itself. Finish re-checks before the reboot. The lock is released on every
 * way out of a session except success, where it is held until the reboot.
 *
 * The flash, the metadata and the image checks are the real ones (flash HAL,
 * boot_meta, bms_image); the contactor interlock comes in through ota_env_t.
 */
#ifndef OTA_PROTO_H
#define OTA_PROTO_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_image.h"
#include "boot_meta.h"
#include "can_if.h"

#define OTA_DATA_BYTES    7u                                  /* image bytes per data frame */
#define OTA_WINDOW_FRAMES 32u                                 /* frames per ACK             */
#define OTA_BLOCK_BYTES   (OTA_WINDOW_FRAMES * OTA_DATA_BYTES) /* 224: 28 program units      */

typedef enum {
    OTA_REQ_START  = 1,
    OTA_REQ_FINISH = 2,
    OTA_REQ_ABORT  = 3,
    OTA_REQ_INFO   = 4
} ota_req_t;

typedef enum {
    OTA_RSP_START    = 1,
    OTA_RSP_FINISH   = 2,
    OTA_RSP_ABORT    = 3,
    OTA_RSP_INFO     = 4,
    OTA_RSP_DATA_ACK = 5
} ota_rsp_t;

/* Values are transmitted (BMS_OtaResponse.OtaStatus). */
typedef enum {
    OTA_OK = 0,
    OTA_ERR_NOT_SAFE   = 1,  /* contactors not open: the update lock was refused  */
    OTA_ERR_BUSY       = 2,  /* running image on trial, or an update already staged */
    OTA_ERR_BAD_SLOT   = 3,  /* target is not the inactive slot                    */
    OTA_ERR_TOO_LARGE  = 4,  /* image size does not fit the slot                   */
    OTA_ERR_DOWNGRADE  = 5,  /* version below the anti-rollback floor              */
    OTA_ERR_SEQUENCE   = 6,  /* DataAck only: resend from OtaNextFrame             */
    OTA_ERR_CRC        = 7,  /* payload CRC mismatch: corrupted transfer           */
    OTA_ERR_SIGNATURE  = 8,  /* not signed with our key, or modified               */
    OTA_ERR_BAD_IMAGE  = 9,  /* not a valid image for that slot / malformed request */
    OTA_ERR_FLASH      = 10, /* erase/program failed                               */
    OTA_ERR_NO_SESSION = 11, /* Finish without a Start                             */
    OTA_ERR_TIMEOUT    = 12, /* session abandoned (sent with an Abort response)    */
    OTA_ERR_INCOMPLETE = 13  /* Finish before every byte arrived                   */
} ota_status_t;

/* The contactor interlock, provided by the update task. */
typedef struct {
    bool (*lock)(void *ctx);             /* take the update lock: true if granted (OPEN)  */
    void (*unlock)(void *ctx);           /* end of session: CLOSE allowed again           */
    bool (*safe_to_activate)(void *ctx); /* re-check at Finish: still OPEN, no fault      */
    void *ctx;
} ota_env_t;

typedef struct {
    uint8_t                  running_slot;
    uint32_t                 running_version;
    uint32_t                 slot_base[BOOT_SLOT_COUNT];
    uint32_t                 slot_size;
    uint32_t                 ram_start;     /* for the image's vector-table check */
    uint32_t                 ram_end;
    const uint8_t           *public_key;
    const boot_meta_store_t *store;
    uint32_t                 session_timeout_ms;
    uint8_t                  boot_reason;   /* reported by GetInfo */
    uint8_t                  boot_attempt;
} ota_config_t;

typedef enum {
    OTA_EV_NONE = 0,
    OTA_EV_STARTED,    /* session accepted, slot erased                    */
    OTA_EV_REFUSED,    /* request refused; see status                       */
    OTA_EV_RETRANSMIT, /* asked the tester to resend from next_frame        */
    OTA_EV_STAGED,     /* verified and staged: reboot now                   */
    OTA_EV_FAILED,     /* session ended with an error; see status           */
    OTA_EV_ABORTED,    /* tester aborted                                    */
    OTA_EV_TIMEOUT     /* session went quiet                                */
} ota_event_t;

typedef struct {
    bool         respond;  /* send `response` */
    can_frame_t  response;
    ota_event_t  event;
    ota_status_t status;
    bool         reboot;   /* image staged: reboot into it */
} ota_output_t;

typedef enum { OTA_IDLE, OTA_RECEIVING } ota_state_t;

typedef struct {
    ota_config_t      cfg;
    ota_env_t         env;
    boot_meta_view_t *meta;       /* shared with the task (it confirms images) */
    ota_state_t       state;
    /* current session */
    uint8_t           target_slot;
    uint32_t          image_size;
    uint32_t          image_version;
    uint32_t          next_frame;
    uint32_t          total_frames;
    uint32_t          started_ms;
    uint32_t          last_rx_ms;
    bool              nack_sent;  /* one Sequence NACK per gap */
    uint32_t          retransmit_requests;
    uint32_t          block_offset;
    uint32_t          block_len;
    uint8_t           block[OTA_BLOCK_BYTES];
} ota_t;

void ota_init(ota_t *o, const ota_config_t *cfg, const ota_env_t *env, boot_meta_view_t *meta);

/* A frame on BMS_ID_OTA_REQUEST or BMS_ID_OTA_DATA. */
void ota_handle_frame(ota_t *o, const can_frame_t *f, uint32_t now_ms, ota_output_t *out);

/* Call regularly: abandons a session that has gone quiet. */
void ota_poll(ota_t *o, uint32_t now_ms, ota_output_t *out);

const char *ota_status_name(ota_status_t s);

#endif /* OTA_PROTO_H */
