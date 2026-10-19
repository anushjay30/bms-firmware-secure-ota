/*
 * bms_msgs.h - Pack/unpack of every BMS CAN message (pure logic).
 *
 * Must stay bit-exact with dbc/bms.dbc. All signals are little-endian
 * (Intel byte order), which is what the Cortex-M3 uses natively too.
 * The scenario tests decode everything the firmware sends with cantools and
 * the DBC, so any mismatch between this file and the DBC shows up there.
 *
 * Identifier map (11-bit standard IDs, lower ID = wins arbitration):
 *   0x050  BMS_FaultEvent    BMS  -> all   on event
 *   0x060  VCU_Command       VCU  -> BMS   on demand, E2E protected
 *   0x100  BMS_Status        BMS  -> VCU   100 ms + on state change, E2E protected
 *   0x200  SIM_PackMeas      SIM  -> BMS   10 ms
 *   0x210  SIM_CellVoltages  SIM  -> BMS   50 ms, multiplexed (4 pages x 3 cells)
 *   0x220  SIM_CellTemps     SIM  -> BMS   50 ms, multiplexed (4 pages x 3 cells)
 *   0x6A0  BMS_TestHook      test -> BMS   debug only (BMS_ENABLE_TEST_HOOKS)
 *   0x6C0  BMS_OtaRequest    test -> BMS   firmware update: start/finish/abort/info
 *   0x6C1  BMS_OtaData       test -> BMS   firmware update: image data
 *   0x6C8  BMS_OtaResponse   BMS  -> test  firmware update: replies, window ACK/NACK
 * The update IDs are the highest (= lowest priority) on the bus, so a
 * transfer can never delay measurements, commands or status.
 */
#ifndef BMS_MSGS_H
#define BMS_MSGS_H

#include <stdbool.h>
#include <stdint.h>

#include "bms_types.h"
#include "can_if.h"
#include "e2e.h"
#include "meas.h"

#define BMS_ID_FAULT_EVENT   0x050u
#define BMS_ID_VCU_COMMAND   0x060u
#define BMS_ID_STATUS        0x100u
#define BMS_ID_PACK_MEAS     0x200u
#define BMS_ID_CELL_VOLTAGES 0x210u
#define BMS_ID_CELL_TEMPS    0x220u
#define BMS_ID_TEST_HOOK     0x6A0u
#define BMS_ID_OTA_REQUEST   0x6C0u
#define BMS_ID_OTA_DATA      0x6C1u
#define BMS_ID_OTA_RESPONSE  0x6C8u

typedef struct {
    bms_state_t state;
    bool        meas_valid;
    uint8_t     faults_latched;
    uint8_t     faults_active;
    uint16_t    soc_pct_x100;
    uint16_t    pack_mv;
} bms_status_msg_t;

typedef enum {
    BMS_HOOK_NONE             = 0,
    BMS_HOOK_STALL_TASK       = 1, /* make one task stop checking in for duration_ms */
    BMS_HOOK_REPORT_STACKS    = 2, /* log every task's stack high-water mark        */
    BMS_HOOK_REBOOT           = 3, /* full system reset (fresh firmware for tests)  */
    BMS_HOOK_FAULT            = 4, /* deliberately fault, to exercise the handler   */
    BMS_HOOK_FLASH_FAIL_WRITE = 5, /* next flash program fails after param bytes    */
    BMS_HOOK_FLASH_POWER_LOSS = 6  /* power is cut after param more programmed bytes */
} bms_hook_id_t;

typedef struct {
    bms_hook_id_t hook;
    uint8_t       task_id;
    uint16_t      duration_ms;
    uint32_t      param;
} bms_test_hook_msg_t;

/* ---- BMS -> bus ---------------------------------------------------------- */
/* Adds the E2E CRC and alive counter (advances *counter). */
void bms_msg_pack_status(const bms_status_msg_t *m, uint8_t *counter, can_frame_t *f);
void bms_msg_pack_fault_event(const bms_fault_event_t *e, uint8_t faults_latched,
                              uint8_t event_counter, can_frame_t *f);

/* ---- bus -> BMS ---------------------------------------------------------- */
bool bms_msg_unpack_cell_voltages(const can_frame_t *f, uint8_t *page, uint16_t mv[MEAS_CELLS_PER_PAGE]);
bool bms_msg_unpack_cell_temps(const can_frame_t *f, uint8_t *page, int16_t dc[MEAS_CELLS_PER_PAGE]);
bool bms_msg_unpack_pack_meas(const can_frame_t *f, int32_t *pack_ma, uint16_t *link_mv);
/* Verifies E2E first; *cmd is only written when the result is E2E_OK. */
e2e_result_t bms_msg_unpack_command(const can_frame_t *f, e2e_rx_state_t *e2e, bms_cmd_t *cmd);
bool bms_msg_unpack_test_hook(const can_frame_t *f, bms_test_hook_msg_t *m);

/* Test helper / VCU side: build a protected command frame. */
void bms_msg_pack_command(bms_cmd_t cmd, uint8_t *counter, can_frame_t *f);

#endif /* BMS_MSGS_H */
