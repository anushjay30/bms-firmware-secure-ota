#include "slcan.h"

#include <string.h>

#define SLCAN_CR  '\r'
#define SLCAN_LF  '\n'
#define SLCAN_BEL '\a'

#define STD_ID_MAX 0x7FFu

static const char s_hex[] = "0123456789ABCDEF";

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10; /* hosts differ; accept both cases */
    }
    return -1;
}

static bool parse_hex(const char *s, size_t n, uint32_t *out)
{
    uint32_t v = 0u;
    for (size_t i = 0; i < n; i++) {
        const int h = hex_value(s[i]);
        if (h < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return true;
}

static void set_error(slcan_event_t *ev, slcan_error_t err)
{
    ev->type = SLCAN_EV_ERROR;
    ev->error = err;
}

/* "tIIIL" + 2*L data digits, optionally followed by a 4-digit timestamp
 * (sent by adapters with timestamps enabled; accepted and ignored). */
static void decode_std_frame(const char *line, size_t len, slcan_event_t *ev)
{
    uint32_t id;
    if (len < 5u || !parse_hex(&line[1], 3u, &id) || id > STD_ID_MAX) {
        set_error(ev, SLCAN_ERR_SYNTAX);
        return;
    }
    if (line[4] < '0' || line[4] > '8') {
        set_error(ev, SLCAN_ERR_SYNTAX); /* also rejects CAN FD length codes 9..F */
        return;
    }
    const uint8_t dlc = (uint8_t)(line[4] - '0');
    const size_t expected = 5u + 2u * dlc;
    if (len != expected && len != expected + 4u) {
        set_error(ev, SLCAN_ERR_SYNTAX);
        return;
    }

    can_frame_t f;
    memset(&f, 0, sizeof f);
    f.id = id;
    f.dlc = dlc;
    for (uint8_t i = 0; i < dlc; i++) {
        uint32_t byte;
        if (!parse_hex(&line[5u + 2u * i], 2u, &byte)) {
            set_error(ev, SLCAN_ERR_SYNTAX);
            return;
        }
        f.data[i] = (uint8_t)byte;
    }
    if (len == expected + 4u) {
        uint32_t timestamp;
        if (!parse_hex(&line[expected], 4u, &timestamp)) {
            set_error(ev, SLCAN_ERR_SYNTAX);
            return;
        }
    }
    ev->type = SLCAN_EV_FRAME;
    ev->frame = f;
}

void slcan_decode_line(const char *line, size_t len, slcan_event_t *ev)
{
    ev->error = SLCAN_ERR_NONE;
    ev->cmd = '\0';
    ev->arg_len = 0u;

    if (len == 0u || len > SLCAN_MAX_LINE) {
        set_error(ev, len == 0u ? SLCAN_ERR_SYNTAX : SLCAN_ERR_OVERFLOW);
        return;
    }
    const char c = line[0];
    if (c == 't') {
        decode_std_frame(line, len, ev);
    } else if (c == 'T' || c == 'r' || c == 'R') {
        /* Extended IDs and remote frames are valid slcan but the BMS only
         * speaks classic 11-bit data frames: count them, do not deliver. */
        set_error(ev, SLCAN_ERR_UNSUPPORTED);
    } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        ev->type = SLCAN_EV_COMMAND;
        ev->cmd = c;
        ev->arg_len = (uint8_t)(len - 1u);
        memcpy(ev->arg, &line[1], len - 1u);
    } else {
        set_error(ev, SLCAN_ERR_SYNTAX); /* noise: digits, punctuation, NUL, ... */
    }
}

void slcan_parser_init(slcan_parser_t *p)
{
    p->len = 0u;
    p->overflow = false;
}

bool slcan_parser_feed(slcan_parser_t *p, uint8_t byte, slcan_event_t *ev)
{
    if (byte == (uint8_t)SLCAN_CR || byte == (uint8_t)SLCAN_LF || byte == (uint8_t)SLCAN_BEL) {
        const bool overflowed = p->overflow;
        const uint8_t len = p->len;
        p->len = 0u;
        p->overflow = false;
        if (overflowed) {
            /* Report the bad line once, at its end; the next byte starts a
             * fresh line. This is how the parser resynchronises after lost
             * bytes or garbage. */
            set_error(ev, SLCAN_ERR_OVERFLOW);
            ev->cmd = '\0';
            ev->arg_len = 0u;
            return true;
        }
        if (len == 0u) {
            return false; /* empty line, e.g. the LF of CRLF */
        }
        slcan_decode_line(p->line, len, ev);
        return true;
    }

    if (p->overflow) {
        return false;
    }
    if (p->len >= SLCAN_MAX_LINE) {
        p->overflow = true; /* drop the rest of this line */
        return false;
    }
    p->line[p->len++] = (char)byte;
    return false;
}

size_t slcan_encode_frame(const can_frame_t *frame, char *buf, size_t cap)
{
    if (frame->id > STD_ID_MAX || frame->dlc > CAN_MAX_DLC) {
        return 0u;
    }
    const size_t n = 5u + 2u * frame->dlc + 1u;
    if (cap < n) {
        return 0u;
    }
    buf[0] = 't';
    buf[1] = s_hex[(frame->id >> 8) & 0xFu];
    buf[2] = s_hex[(frame->id >> 4) & 0xFu];
    buf[3] = s_hex[frame->id & 0xFu];
    buf[4] = (char)('0' + frame->dlc);
    for (uint8_t i = 0; i < frame->dlc; i++) {
        buf[5u + 2u * i] = s_hex[frame->data[i] >> 4];
        buf[6u + 2u * i] = s_hex[frame->data[i] & 0xFu];
    }
    buf[n - 1u] = SLCAN_CR;
    return n;
}

static size_t reply_text(char *reply, size_t cap, const char *text)
{
    const size_t n = strlen(text);
    if (n + 1u > cap) {
        return 0u;
    }
    memcpy(reply, text, n);
    reply[n] = SLCAN_CR;
    return n + 1u;
}

static size_t reply_ok(char *reply, size_t cap)
{
    return reply_text(reply, cap, "");
}

static size_t reply_error(char *reply, size_t cap)
{
    if (cap < 1u) {
        return 0u;
    }
    reply[0] = SLCAN_BEL;
    return 1u;
}

size_t slcan_device_command(slcan_device_t *dev, const slcan_event_t *ev, char *reply, size_t cap)
{
    if (ev->type == SLCAN_EV_ERROR) {
        return reply_error(reply, cap); /* tell the host its line was rejected */
    }
    if (ev->type != SLCAN_EV_COMMAND) {
        /* Frames get no reply. LAWICEL defines 'z' as a transmit ack, but the
         * Linux slcan driver ignores it, and skipping it halves the device ->
         * host traffic for every received frame. */
        return 0u;
    }

    switch (ev->cmd) {
    case 'O': /* open */
    case 'L': /* open listen-only; the BMS must transmit, so treat as open */
        if (ev->arg_len != 0u) {
            return reply_error(reply, cap);
        }
        dev->open = true;
        return reply_ok(reply, cap);
    case 'C': /* close */
        dev->open = false;
        return reply_ok(reply, cap);
    case 'S': /* S0..S8: 10k..1M bit/s. A UART link has no bit timing; remember it. */
        if (ev->arg_len != 1u || ev->arg[0] < '0' || ev->arg[0] > '8') {
            return reply_error(reply, cap);
        }
        dev->bitrate = ev->arg[0];
        return reply_ok(reply, cap);
    case 'V': /* hardware + software version */
        return reply_text(reply, cap, "V0103");
    case 'N': /* serial number */
        return reply_text(reply, cap, "NBMS1");
    case 'F': /* status flags: no errors, no overruns */
        return reply_text(reply, cap, "F00");
    case 'Z': /* timestamps on/off: accepted, we never add them */
    case 'M': /* acceptance code/mask: filtering is done by ID list instead */
    case 'm':
        return reply_ok(reply, cap);
    default:
        return reply_error(reply, cap);
    }
}
