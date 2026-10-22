/* AVRCP core against the exchange captured between Windows and JBL Live 780NC
 * (captures/btvs_full.pcap, see docs/jbl-live-780nc.md). Where Windows' own
 * answers are in the capture, ours must be byte-identical. */
#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "avrcp.h"

static uint8_t g_out[8][64];
static size_t g_out_len[8];
static int g_sent;
static int g_volume = -1;
static int g_key = -1, g_pressed = -1;

static int on_send(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    if (g_sent < 8 && len <= sizeof(g_out[0])) {
        memcpy(g_out[g_sent], buf, len);
        g_out_len[g_sent] = len;
    }
    g_sent++;
    return 0;
}

static void on_volume(void *ctx, uint8_t v)
{
    (void)ctx;
    g_volume = v;
}

static void on_key(void *ctx, uint8_t op, int pressed)
{
    (void)ctx;
    g_key = op;
    g_pressed = pressed;
}

static const avrcp_ops k_ops = { on_send, on_volume, on_key };

static size_t hex(const char *s, uint8_t *out)
{
    size_t n = 0;
    while (s[0] && s[1]) {
        char b[3] = { s[0], s[1], 0 };
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
        s += 2;
    }
    return n;
}

/* Feed one packet from the headphones; returns how many packets we sent. */
static int feed(avrcp *a, const char *h)
{
    uint8_t buf[64];
    size_t n = hex(h, buf);
    g_sent = 0;
    CHECK_EQ(avrcp_input(a, buf, n), 0);
    return g_sent;
}

static int sent_is(int i, const char *h)
{
    uint8_t buf[64];
    size_t n = hex(h, buf);
    return g_out_len[i] == n && memcmp(g_out[i], buf, n) == 0;
}

void test_avrcp(void)
{
    avrcp a;

    avrcp_init(&a, &k_ops, NULL);
    CHECK_EQ(avrcp_set_volume(&a, 50), -1); /* not connected */

    /* CT: our GetCapabilities(EVENTS) on connect */
    g_sent = 0;
    avrcp_connected(&a);
    CHECK_EQ(g_sent, 1);
    CHECK(sent_is(0, "00110e0148000019581000000103"));

    /* TG: the headphones ask ours (capture: Windows answers 6 events, we 2) */
    CHECK_EQ(feed(&a, "10110e0148000019581000000103"), 1);
    CHECK(sent_is(0, "12110e0c48000019581000000403020102"));

    /* CT: their events include VOLUME_CHANGED -> register for it */
    CHECK_EQ(feed(&a, "02110e0c4800001958100000040302060d"), 1);
    CHECK(sent_is(0, "10110e034800001958310000050d00000000"));
    CHECK_EQ(a.abs_volume, AVRCP_ABSVOL_YES);

    /* TG: PLAYBACK_STATUS and TRACK_CHANGED interim, byte-identical to Windows */
    CHECK_EQ(feed(&a, "20110e034800001958310000050100000000"), 1);
    CHECK(sent_is(0, "22110e0f4800001958310000020102"));
    CHECK_EQ(feed(&a, "30110e034800001958310000050200000000"), 1);
    CHECK(sent_is(0, "32110e0f480000195831000009020000000000000000"));

    /* CT: interim volume 71 (0x47), as in the capture */
    CHECK_EQ(feed(&a, "12110e0f4800001958310000020d47"), 0);
    CHECK_EQ(g_volume, 71);

    /* TG: metadata request -> no attributes */
    CHECK_EQ(feed(&a, "40110e014800001958200000110000000000000000020000000100000002"), 1);
    CHECK(sent_is(0, "42110e0c48000019582000000100"));

    /* TG: playback starts -> CHANGED on the registered label, once */
    g_sent = 0;
    avrcp_set_playing(&a, 1);
    CHECK_EQ(g_sent, 1);
    CHECK(sent_is(0, "22110e0d4800001958310000020101"));
    g_sent = 0;
    avrcp_set_playing(&a, 0);
    CHECK_EQ(g_sent, 0); /* not registered again yet */

    /* CT: SetAbsoluteVolume, a second value while the first is pending */
    g_sent = 0;
    CHECK_EQ(avrcp_set_volume(&a, 100), 0);
    CHECK_EQ(g_sent, 1);
    CHECK(sent_is(0, "20110e0048000019585000000164"));
    g_sent = 0;
    CHECK_EQ(avrcp_set_volume(&a, 90), 0);
    CHECK_EQ(g_sent, 0);
    CHECK_EQ(feed(&a, "22110e0948000019585000000164"), 1); /* accepted 100 -> now 90 */
    CHECK(sent_is(0, "30110e004800001958500000015a"));
    CHECK_EQ(feed(&a, "32110e094800001958500000015a"), 0);
    CHECK_EQ(a.volume, 90);

    /* CT: volume changed on the headphones -> report and register again */
    g_volume = -1;
    CHECK_EQ(feed(&a, "12110e0d4800001958310000020d30"), 1);
    CHECK_EQ(g_volume, 48);
    CHECK(sent_is(0, "40110e034800001958310000050d00000000"));

    /* TG: buttons */
    CHECK_EQ(feed(&a, "50110e00487c440000"), 1);
    CHECK(sent_is(0, "52110e09487c440000"));
    CHECK_EQ(g_key, AVRCP_OP_PLAY);
    CHECK_EQ(g_pressed, 1);
    CHECK_EQ(feed(&a, "60110e00487cc40000"), 1);
    CHECK_EQ(g_pressed, 0);
    CHECK_EQ(feed(&a, "70110e00487c010000"), 1); /* unknown op: not implemented */
    CHECK(sent_is(0, "72110e08487c010000"));

    /* TG: unit info, unknown PDU, unknown event, unknown PID */
    CHECK_EQ(feed(&a, "80110e01ff30ffffffffff"), 1);
    CHECK(sent_is(0, "82110e0cff300748ffffff"));
    CHECK_EQ(feed(&a, "90110e014800001958600000020001"), 1);
    CHECK(sent_is(0, "92110e0a48000019586000000100"));
    CHECK_EQ(feed(&a, "a0110e034800001958310000050d00000000"), 1);
    CHECK(sent_is(0, "a2110e0a48000019583100000101"));
    CHECK_EQ(feed(&a, "b01111014800"), 1);
    CHECK(sent_is(0, "b31111"));

    /* malformed */
    {
        uint8_t bad[] = { 0x10, 0x11, 0x0e, 0x01, 0x48, 0x00, 0x00, 0x19, 0x58, 0x10, 0x00, 0x00, 0x09 };
        CHECK_EQ(avrcp_input(&a, bad, sizeof(bad)), -1); /* parameter length beyond the packet */
        CHECK_EQ(avrcp_input(&a, bad, 2), -1);
    }

    /* headphones without absolute volume */
    avrcp_init(&a, &k_ops, NULL);
    avrcp_connected(&a);
    CHECK_EQ(feed(&a, "02110e0c480000195810000003030106"), 0);
    CHECK_EQ(a.abs_volume, AVRCP_ABSVOL_NO);
    CHECK_EQ(avrcp_set_volume(&a, 10), -1);

    /* no answer to GetCapabilities */
    avrcp_init(&a, &k_ops, NULL);
    avrcp_connected(&a);
    avrcp_timeout(&a);
    CHECK_EQ(a.abs_volume, AVRCP_ABSVOL_NO);
}
