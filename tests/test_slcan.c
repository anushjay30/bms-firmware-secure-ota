#include <string.h>

#include "slcan.h"
#include "unity.h"

static slcan_parser_t parser;

void setUp(void)
{
    slcan_parser_init(&parser);
}

void tearDown(void) {}

/* Feed a string; collect up to max events. Returns the number of events. */
static int feed(const char *s, size_t n, slcan_event_t *events, int max)
{
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        slcan_event_t ev;
        if (slcan_parser_feed(&parser, (uint8_t)s[i], &ev) && count < max) {
            events[count++] = ev;
        }
    }
    return count;
}

#define FEED(str, evs) feed((str), sizeof(str) - 1u, (evs), (int)(sizeof(evs) / sizeof((evs)[0])))

static can_frame_t make_frame(uint32_t id, uint8_t dlc)
{
    can_frame_t f = { .id = id, .dlc = dlc, .data = { 0 } };
    for (uint8_t i = 0; i < dlc; i++) {
        f.data[i] = (uint8_t)(0x11u * (i + 1u));
    }
    return f;
}

/* ---- Encoder ---------------------------------------------------------------- */

void test_encode_standard_frame(void)
{
    const can_frame_t f = { .id = 0x123, .dlc = 4, .data = { 0xDE, 0xAD, 0xBE, 0xEF } };
    char buf[SLCAN_MAX_FRAME_TEXT];
    const size_t n = slcan_encode_frame(&f, buf, sizeof buf);
    TEST_ASSERT_EQUAL_UINT(14, n);
    TEST_ASSERT_EQUAL_MEMORY("t1234DEADBEEF\r", buf, n);
}

void test_encode_zero_length_and_max_id(void)
{
    const can_frame_t f = { .id = 0x7FF, .dlc = 0 };
    char buf[8];
    TEST_ASSERT_EQUAL_UINT(6, slcan_encode_frame(&f, buf, sizeof buf));
    TEST_ASSERT_EQUAL_MEMORY("t7FF0\r", buf, 6);
}

void test_encode_rejects_invalid_frames_and_small_buffers(void)
{
    char buf[SLCAN_MAX_FRAME_TEXT];
    const can_frame_t big_id = { .id = 0x800, .dlc = 1 };
    const can_frame_t big_dlc = { .id = 0x100, .dlc = 9 };
    const can_frame_t ok = make_frame(0x100, 8);
    TEST_ASSERT_EQUAL_UINT(0, slcan_encode_frame(&big_id, buf, sizeof buf));
    TEST_ASSERT_EQUAL_UINT(0, slcan_encode_frame(&big_dlc, buf, sizeof buf));
    TEST_ASSERT_EQUAL_UINT(0, slcan_encode_frame(&ok, buf, 21)); /* needs 22 */
    TEST_ASSERT_EQUAL_UINT(22, slcan_encode_frame(&ok, buf, 22));
}

void test_encode_decode_roundtrip_every_length(void)
{
    for (uint8_t dlc = 0; dlc <= 8u; dlc++) {
        const can_frame_t in = make_frame(0x5A0u + dlc, dlc);
        char buf[SLCAN_MAX_FRAME_TEXT];
        const size_t n = slcan_encode_frame(&in, buf, sizeof buf);
        slcan_event_t ev[2];
        TEST_ASSERT_EQUAL_INT(1, feed(buf, n, ev, 2));
        TEST_ASSERT_EQUAL(SLCAN_EV_FRAME, ev[0].type);
        TEST_ASSERT_EQUAL_HEX32(in.id, ev[0].frame.id);
        TEST_ASSERT_EQUAL_UINT8(dlc, ev[0].frame.dlc);
        TEST_ASSERT_EQUAL_MEMORY(in.data, ev[0].frame.data, CAN_MAX_DLC);
    }
}

/* ---- Parser: framing ------------------------------------------------------ */

void test_frame_split_across_many_reads(void)
{
    const char *line = "t2108003C0E3D0E3E0E00\r";
    slcan_event_t ev = { 0 };
    bool got = false;
    for (size_t i = 0; i < strlen(line); i++) { /* one byte per "UART read" */
        const bool done = slcan_parser_feed(&parser, (uint8_t)line[i], &ev);
        TEST_ASSERT_EQUAL(i == strlen(line) - 1u, done);
        got = got || done;
    }
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL(SLCAN_EV_FRAME, ev.type);
    TEST_ASSERT_EQUAL_HEX32(0x210, ev.frame.id);
    TEST_ASSERT_EQUAL_HEX8(0x3C, ev.frame.data[1]);
}

void test_several_frames_in_one_read(void)
{
    slcan_event_t ev[4];
    TEST_ASSERT_EQUAL_INT(3, FEED("t0601AA\rt1000\rt7FF211FF\r", ev));
    TEST_ASSERT_EQUAL_HEX32(0x060, ev[0].frame.id);
    TEST_ASSERT_EQUAL_HEX32(0x100, ev[1].frame.id);
    TEST_ASSERT_EQUAL_HEX8(0xFF, ev[2].frame.data[1]);
}

void test_lowercase_hex_crlf_and_timestamps_accepted(void)
{
    slcan_event_t ev[4];
    /* lowercase hex; CRLF line end (the LF is an empty line: no event);
     * trailing 4-digit timestamp */
    TEST_ASSERT_EQUAL_INT(2, FEED("t1ab2beef\r\nt1230ABCD\r", ev));
    TEST_ASSERT_EQUAL(SLCAN_EV_FRAME, ev[0].type);
    TEST_ASSERT_EQUAL_HEX32(0x1AB, ev[0].frame.id);
    TEST_ASSERT_EQUAL_HEX8(0xEF, ev[0].frame.data[1]);
    TEST_ASSERT_EQUAL(SLCAN_EV_FRAME, ev[1].type);
    TEST_ASSERT_EQUAL_UINT8(0, ev[1].frame.dlc);
}

/* ---- Parser: malformed input ---------------------------------------------- */

static void assert_single_error(const char *s, size_t n, slcan_error_t expected)
{
    slcan_event_t ev[2];
    TEST_ASSERT_EQUAL_INT(1, feed(s, n, ev, 2));
    TEST_ASSERT_EQUAL(SLCAN_EV_ERROR, ev[0].type);
    TEST_ASSERT_EQUAL(expected, ev[0].error);
}

#define ASSERT_ERROR(str, err) assert_single_error((str), sizeof(str) - 1u, (err))

void test_bad_hex_digit(void)
{
    ASSERT_ERROR("t12G0\r", SLCAN_ERR_SYNTAX);      /* in the ID   */
    ASSERT_ERROR("t1231XY\r", SLCAN_ERR_SYNTAX);    /* in the data */
}

void test_wrong_length_for_dlc(void)
{
    ASSERT_ERROR("t1232AA\r", SLCAN_ERR_SYNTAX);    /* says 2 bytes, has 1 */
    ASSERT_ERROR("t1231AABB\r", SLCAN_ERR_SYNTAX);  /* says 1 byte, has 2  */
    ASSERT_ERROR("t12\r", SLCAN_ERR_SYNTAX);        /* truncated           */
}

void test_dlc_above_eight_and_id_above_11_bits(void)
{
    ASSERT_ERROR("t1239\r", SLCAN_ERR_SYNTAX);      /* CAN FD length code */
    ASSERT_ERROR("t8000\r", SLCAN_ERR_SYNTAX);      /* 0x800 > 0x7FF      */
}

void test_extended_and_remote_frames_are_unsupported(void)
{
    ASSERT_ERROR("T123456780\r", SLCAN_ERR_UNSUPPORTED);
    ASSERT_ERROR("r1230\r", SLCAN_ERR_UNSUPPORTED);
}

void test_garbage_line_is_a_syntax_error(void)
{
    ASSERT_ERROR("#!?\r", SLCAN_ERR_SYNTAX);
    const char with_nul[] = { 't', '1', '\0', '3', '0', '\r' };
    assert_single_error(with_nul, sizeof with_nul, SLCAN_ERR_SYNTAX);
}

void test_overlong_line_reported_once_then_parser_resyncs(void)
{
    char input[200];
    memset(input, 'A', 120); /* a lost CR: 120 chars without a line end */
    memcpy(&input[120], "\rt1001AB\r", 9);
    slcan_event_t ev[4];
    TEST_ASSERT_EQUAL_INT(2, feed(input, 129, ev, 4));
    TEST_ASSERT_EQUAL(SLCAN_EV_ERROR, ev[0].type);
    TEST_ASSERT_EQUAL(SLCAN_ERR_OVERFLOW, ev[0].error);
    TEST_ASSERT_EQUAL(SLCAN_EV_FRAME, ev[1].type); /* next line decodes fine */
    TEST_ASSERT_EQUAL_HEX8(0xAB, ev[1].frame.data[0]);
}

void test_longest_legal_line_is_not_an_overflow(void)
{
    slcan_event_t ev[2];
    /* 8 data bytes + timestamp = 25 characters before the CR */
    TEST_ASSERT_EQUAL_INT(1, FEED("t12380011223344556677ABCD\r", ev));
    TEST_ASSERT_EQUAL(SLCAN_EV_FRAME, ev[0].type);
}

void test_empty_lines_produce_no_events(void)
{
    slcan_event_t ev[2];
    TEST_ASSERT_EQUAL_INT(0, FEED("\r\r\n\n\r", ev));
}

/* ---- Commands --------------------------------------------------------------- */

void test_command_lines_are_reported_with_arguments(void)
{
    slcan_event_t ev[3];
    TEST_ASSERT_EQUAL_INT(3, FEED("S6\rO\rV\r", ev));
    TEST_ASSERT_EQUAL(SLCAN_EV_COMMAND, ev[0].type);
    TEST_ASSERT_EQUAL_CHAR('S', ev[0].cmd);
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].arg_len);
    TEST_ASSERT_EQUAL_CHAR('6', ev[0].arg[0]);
    TEST_ASSERT_EQUAL_CHAR('O', ev[1].cmd);
    TEST_ASSERT_EQUAL_UINT8(0, ev[1].arg_len);
}

void test_device_replies_ok_error_and_tracks_state(void)
{
    slcan_device_t dev = { .open = true, .bitrate = '6' };
    char reply[16];
    slcan_event_t ev[1];

    FEED("C\r", ev);
    TEST_ASSERT_EQUAL_UINT(1, slcan_device_command(&dev, &ev[0], reply, sizeof reply));
    TEST_ASSERT_EQUAL_CHAR('\r', reply[0]);
    TEST_ASSERT_FALSE(dev.open);

    FEED("S4\r", ev);
    TEST_ASSERT_EQUAL_UINT(1, slcan_device_command(&dev, &ev[0], reply, sizeof reply));
    TEST_ASSERT_EQUAL_CHAR('4', dev.bitrate);

    FEED("S9\r", ev); /* no such bitrate */
    TEST_ASSERT_EQUAL_UINT(1, slcan_device_command(&dev, &ev[0], reply, sizeof reply));
    TEST_ASSERT_EQUAL_CHAR('\a', reply[0]);

    FEED("O\r", ev);
    (void)slcan_device_command(&dev, &ev[0], reply, sizeof reply);
    TEST_ASSERT_TRUE(dev.open);

    FEED("V\r", ev);
    const size_t n = slcan_device_command(&dev, &ev[0], reply, sizeof reply);
    TEST_ASSERT_EQUAL_MEMORY("V0103\r", reply, n);

    FEED("Q\r", ev); /* unknown command */
    TEST_ASSERT_EQUAL_UINT(1, slcan_device_command(&dev, &ev[0], reply, sizeof reply));
    TEST_ASSERT_EQUAL_CHAR('\a', reply[0]);
}

void test_device_nacks_malformed_lines_and_does_not_ack_frames(void)
{
    slcan_device_t dev = { .open = true, .bitrate = '6' };
    char reply[16];
    slcan_event_t ev[1];

    FEED("t12G0\r", ev);
    TEST_ASSERT_EQUAL_UINT(1, slcan_device_command(&dev, &ev[0], reply, sizeof reply));
    TEST_ASSERT_EQUAL_CHAR('\a', reply[0]);

    FEED("t1230\r", ev);
    TEST_ASSERT_EQUAL_UINT(0, slcan_device_command(&dev, &ev[0], reply, sizeof reply));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_standard_frame);
    RUN_TEST(test_encode_zero_length_and_max_id);
    RUN_TEST(test_encode_rejects_invalid_frames_and_small_buffers);
    RUN_TEST(test_encode_decode_roundtrip_every_length);
    RUN_TEST(test_frame_split_across_many_reads);
    RUN_TEST(test_several_frames_in_one_read);
    RUN_TEST(test_lowercase_hex_crlf_and_timestamps_accepted);
    RUN_TEST(test_bad_hex_digit);
    RUN_TEST(test_wrong_length_for_dlc);
    RUN_TEST(test_dlc_above_eight_and_id_above_11_bits);
    RUN_TEST(test_extended_and_remote_frames_are_unsupported);
    RUN_TEST(test_garbage_line_is_a_syntax_error);
    RUN_TEST(test_overlong_line_reported_once_then_parser_resyncs);
    RUN_TEST(test_longest_legal_line_is_not_an_overflow);
    RUN_TEST(test_empty_lines_produce_no_events);
    RUN_TEST(test_command_lines_are_reported_with_arguments);
    RUN_TEST(test_device_replies_ok_error_and_tracks_state);
    RUN_TEST(test_device_nacks_malformed_lines_and_does_not_ack_frames);
    return UNITY_END();
}
