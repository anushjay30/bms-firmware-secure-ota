#include <string.h>

#include "bms_msgs.h"
#include "unity.h"

static can_frame_t f;

void setUp(void)
{
    memset(&f, 0, sizeof f);
}

void tearDown(void) {}

void test_status_layout_and_e2e(void)
{
    const bms_status_msg_t m = { .state = BMS_STATE_CLOSED, .meas_valid = true, .faults_latched = 0x04u,
                                 .faults_active = 0x02u, .soc_pct_x100 = 7999u, .pack_mv = 44400u };
    uint8_t counter = 9u;
    bms_msg_pack_status(&m, &counter, &f);

    TEST_ASSERT_EQUAL_HEX32(BMS_ID_STATUS, f.id);
    TEST_ASSERT_EQUAL_UINT8(8, f.dlc);
    TEST_ASSERT_EQUAL_HEX8(0x80u | (2u << 4) | 9u, f.data[1]); /* valid | state | counter */
    TEST_ASSERT_EQUAL_HEX8(0x04, f.data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x02, f.data[3]);
    TEST_ASSERT_EQUAL_HEX8(7999 & 0xFF, f.data[4]);
    TEST_ASSERT_EQUAL_HEX8(7999 >> 8, f.data[5]);
    TEST_ASSERT_EQUAL_HEX8(44400 & 0xFF, f.data[6]);
    TEST_ASSERT_EQUAL_HEX8(44400 >> 8, f.data[7]);

    e2e_rx_state_t rx = { 0 };
    TEST_ASSERT_EQUAL(E2E_OK, e2e_check(&rx, &f));
}

void test_command_roundtrip(void)
{
    uint8_t tx_counter = 0u;
    e2e_rx_state_t rx = { 0 };
    bms_cmd_t cmd = BMS_CMD_NONE;

    bms_msg_pack_command(BMS_CMD_CLOSE, &tx_counter, &f);
    TEST_ASSERT_EQUAL(E2E_OK, bms_msg_unpack_command(&f, &rx, &cmd));
    TEST_ASSERT_EQUAL(BMS_CMD_CLOSE, cmd);

    bms_msg_pack_command(BMS_CMD_RESET, &tx_counter, &f);
    TEST_ASSERT_EQUAL(E2E_OK, bms_msg_unpack_command(&f, &rx, &cmd));
    TEST_ASSERT_EQUAL(BMS_CMD_RESET, cmd);
}

void test_command_with_bad_crc_is_not_decoded(void)
{
    uint8_t tx_counter = 0u;
    e2e_rx_state_t rx = { 0 };
    bms_cmd_t cmd = BMS_CMD_NONE;

    bms_msg_pack_command(BMS_CMD_CLOSE, &tx_counter, &f);
    f.data[0] ^= 0xFFu;
    TEST_ASSERT_EQUAL(E2E_ERR_CRC, bms_msg_unpack_command(&f, &rx, &cmd));
    TEST_ASSERT_EQUAL(BMS_CMD_NONE, cmd);
}

void test_unknown_command_value_maps_to_none(void)
{
    uint8_t tx_counter = 0u;
    e2e_rx_state_t rx = { 0 };
    bms_cmd_t cmd = BMS_CMD_CLOSE;

    bms_msg_pack_command((bms_cmd_t)42, &tx_counter, &f);
    TEST_ASSERT_EQUAL(E2E_OK, bms_msg_unpack_command(&f, &rx, &cmd));
    TEST_ASSERT_EQUAL(BMS_CMD_NONE, cmd);
}

void test_cell_voltage_page_decode(void)
{
    const uint8_t raw[8] = { 2, 0x74, 0x0E, 0x75, 0x0E, 0x76, 0x0E, 0 }; /* 3700, 3701, 3702 */
    f.id = BMS_ID_CELL_VOLTAGES;
    f.dlc = 8u;
    memcpy(f.data, raw, sizeof raw);

    uint8_t page;
    uint16_t mv[MEAS_CELLS_PER_PAGE];
    TEST_ASSERT_TRUE(bms_msg_unpack_cell_voltages(&f, &page, mv));
    TEST_ASSERT_EQUAL_UINT8(2, page);
    TEST_ASSERT_EQUAL_UINT16(3700, mv[0]);
    TEST_ASSERT_EQUAL_UINT16(3702, mv[2]);
}

void test_cell_voltage_rejects_bad_page_and_short_frame(void)
{
    uint8_t page;
    uint16_t mv[MEAS_CELLS_PER_PAGE];
    f.id = BMS_ID_CELL_VOLTAGES;
    f.dlc = 8u;
    f.data[0] = MEAS_PAGES; /* one past the last page */
    TEST_ASSERT_FALSE(bms_msg_unpack_cell_voltages(&f, &page, mv));
    f.data[0] = 0u;
    f.dlc = 6u;
    TEST_ASSERT_FALSE(bms_msg_unpack_cell_voltages(&f, &page, mv));
}

void test_negative_temperature_decode(void)
{
    f.id = BMS_ID_CELL_TEMPS;
    f.dlc = 8u;
    f.data[0] = 1u;
    f.data[1] = 0x9Cu; /* -100 = -10.0 degC, two's complement 0xFF9C */
    f.data[2] = 0xFFu;

    uint8_t page;
    int16_t dc[MEAS_CELLS_PER_PAGE];
    TEST_ASSERT_TRUE(bms_msg_unpack_cell_temps(&f, &page, dc));
    TEST_ASSERT_EQUAL_INT16(-100, dc[0]);
}

void test_pack_meas_charge_current_is_negative(void)
{
    f.id = BMS_ID_PACK_MEAS;
    f.dlc = 8u;
    const int32_t ma = -123456;
    const uint32_t u = (uint32_t)ma;
    for (unsigned i = 0; i < 4u; i++) {
        f.data[i] = (uint8_t)(u >> (8u * i));
    }
    f.data[4] = 0x50u; /* 44112 mV */
    f.data[5] = 0xACu;

    int32_t out_ma;
    uint16_t link;
    TEST_ASSERT_TRUE(bms_msg_unpack_pack_meas(&f, &out_ma, &link));
    TEST_ASSERT_EQUAL_INT32(-123456, out_ma);
    TEST_ASSERT_EQUAL_UINT16(44112, link);
}

void test_wrong_id_is_rejected(void)
{
    int32_t ma;
    uint16_t link;
    f.id = BMS_ID_CELL_TEMPS;
    f.dlc = 8u;
    TEST_ASSERT_FALSE(bms_msg_unpack_pack_meas(&f, &ma, &link));
}

void test_fault_event_layout(void)
{
    const bms_fault_event_t e = { .id = BMS_FAULT_CELL_OT, .cell = 3u, .value = 650 };
    bms_msg_pack_fault_event(&e, 0x04u, 7u, &f);
    TEST_ASSERT_EQUAL_HEX32(BMS_ID_FAULT_EVENT, f.id);
    TEST_ASSERT_EQUAL_UINT8(BMS_FAULT_CELL_OT, f.data[0]);
    TEST_ASSERT_EQUAL_UINT8(3, f.data[1]);
    TEST_ASSERT_EQUAL_HEX8(650 & 0xFF, f.data[2]);
    TEST_ASSERT_EQUAL_HEX8(650 >> 8, f.data[3]);
    TEST_ASSERT_EQUAL_HEX8(0, f.data[5]);
    TEST_ASSERT_EQUAL_HEX8(0x04, f.data[6]);
    TEST_ASSERT_EQUAL_UINT8(7, f.data[7]);
}

void test_test_hook_decode(void)
{
    f.id = BMS_ID_TEST_HOOK;
    f.dlc = 4u;
    f.data[0] = BMS_HOOK_STALL_TASK;
    f.data[1] = 1u;
    f.data[2] = 0xE8u; /* 1000 ms */
    f.data[3] = 0x03u;

    bms_test_hook_msg_t h;
    TEST_ASSERT_TRUE(bms_msg_unpack_test_hook(&f, &h));
    TEST_ASSERT_EQUAL(BMS_HOOK_STALL_TASK, h.hook);
    TEST_ASSERT_EQUAL_UINT8(1, h.task_id);
    TEST_ASSERT_EQUAL_UINT16(1000, h.duration_ms);
}

void test_debug_hooks_decode_and_unknown_ids_are_ignored(void)
{
    bms_test_hook_msg_t h;
    f.id = BMS_ID_TEST_HOOK;
    f.dlc = 4u;

    f.data[0] = BMS_HOOK_REPORT_STACKS;
    TEST_ASSERT_TRUE(bms_msg_unpack_test_hook(&f, &h));
    TEST_ASSERT_EQUAL(BMS_HOOK_REPORT_STACKS, h.hook);

    f.data[0] = BMS_HOOK_REBOOT;
    TEST_ASSERT_TRUE(bms_msg_unpack_test_hook(&f, &h));
    TEST_ASSERT_EQUAL(BMS_HOOK_REBOOT, h.hook);

    f.data[0] = 0x42u; /* not a hook we know: must not map onto any action */
    TEST_ASSERT_TRUE(bms_msg_unpack_test_hook(&f, &h));
    TEST_ASSERT_EQUAL(BMS_HOOK_NONE, h.hook);
}

void test_flash_fault_hook_carries_a_byte_count(void)
{
    f.id = BMS_ID_TEST_HOOK;
    f.dlc = 8u;
    f.data[0] = BMS_HOOK_FLASH_POWER_LOSS;
    f.data[4] = 0x10u; /* 10000, little-endian */
    f.data[5] = 0x27u;

    bms_test_hook_msg_t h;
    TEST_ASSERT_TRUE(bms_msg_unpack_test_hook(&f, &h));
    TEST_ASSERT_EQUAL(BMS_HOOK_FLASH_POWER_LOSS, h.hook);
    TEST_ASSERT_EQUAL_UINT32(10000u, h.param);

    f.dlc = 4u; /* a 4-byte frame from an older tester has no parameter */
    TEST_ASSERT_TRUE(bms_msg_unpack_test_hook(&f, &h));
    TEST_ASSERT_EQUAL_UINT32(0u, h.param);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_status_layout_and_e2e);
    RUN_TEST(test_command_roundtrip);
    RUN_TEST(test_command_with_bad_crc_is_not_decoded);
    RUN_TEST(test_unknown_command_value_maps_to_none);
    RUN_TEST(test_cell_voltage_page_decode);
    RUN_TEST(test_cell_voltage_rejects_bad_page_and_short_frame);
    RUN_TEST(test_negative_temperature_decode);
    RUN_TEST(test_pack_meas_charge_current_is_negative);
    RUN_TEST(test_wrong_id_is_rejected);
    RUN_TEST(test_fault_event_layout);
    RUN_TEST(test_test_hook_decode);
    RUN_TEST(test_debug_hooks_decode_and_unknown_ids_are_ignored);
    RUN_TEST(test_flash_fault_hook_carries_a_byte_count);
    return UNITY_END();
}
