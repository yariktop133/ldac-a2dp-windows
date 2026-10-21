/*
 * Replays the real JBL Live 780NC signaling captured with btvs against
 * Alternative A2DP 1.8.3.1 (docs/jbl-live-780nc.md) and checks that our
 * source sends byte-identical commands.
 */
#include "avdtp_source.h"
#include "test.h"

#include <string.h>

typedef struct replay {
    uint8_t sent[8][64];
    size_t sent_len[8];
    int n_sent;
    int media_open;
    uint16_t delay;
} replay;

static int on_send(void *ctx, const uint8_t *buf, size_t len)
{
    replay *r = ctx;
    if (r->n_sent < 8 && len <= sizeof(r->sent[0])) {
        memcpy(r->sent[r->n_sent], buf, len);
        r->sent_len[r->n_sent] = len;
    }
    r->n_sent++;
    return 0;
}

static void on_media_open(void *ctx, int initiate)
{
    ((replay *)ctx)->media_open = initiate ? 1 : -1;
}

static void on_delay(void *ctx, uint16_t d)
{
    ((replay *)ctx)->delay = d;
}

static size_t hex(const char *s, uint8_t *out)
{
    size_t n = 0;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

/* Expect the last command sent by the source to equal `want_hex`. */
static void expect_sent(replay *r, int idx, const char *want_hex)
{
    uint8_t want[64];
    size_t n = hex(want_hex, want);
    CHECK_EQ(r->n_sent, idx + 1);
    CHECK_EQ(r->sent_len[idx], n);
    CHECK(r->sent_len[idx] == n && memcmp(r->sent[idx], want, n) == 0);
}

static void feed(avdtp_source *s, const char *pkt_hex)
{
    uint8_t buf[64];
    size_t n = hex(pkt_hex, buf);
    CHECK_EQ(avdtp_src_on_signal(s, buf, n), 0);
}

void test_replay(void)
{
    static const avdtp_src_ops ops = { on_send, on_media_open, NULL, NULL, NULL, on_delay };
    avdtp_source s;
    replay r;
    a2dp_prefs p;

    memset(&r, 0, sizeof(r));
    a2dp_prefs_default(&p); /* prefers 96 kHz, like the captured session */
    avdtp_src_init(&s, &ops, &r, &p);
    s.auto_start = 0;

    avdtp_src_connect(&s);
    expect_sent(&r, 0, "0001");
    feed(&s, "0201040808080c08"); /* SBC, AAC, LDAC sinks */
    expect_sent(&r, 1, "100c04");
    feed(&s, "120c0100070600003fff02350800");
    expect_sent(&r, 2, "200c08");
    feed(&s, "220c01000708000280018c8400000800");
    expect_sent(&r, 3, "300c0c");
    feed(&s, "320c0100070a00ff2d010000aa003c070800");
    /* LDAC 96 kHz stereo on SEID 3, from our SEID 1, with delay reporting */
    expect_sent(&r, 4, "40030c040100070a00ff2d010000aa0004010800");
    feed(&s, "4203");
    expect_sent(&r, 5, "50060c");
    CHECK_EQ(s.state, AVDTP_SRC_OPENING);

    feed(&s, "000d0405dc"); /* DELAYREPORT 150 ms */
    expect_sent(&r, 6, "020d");
    CHECK_EQ(r.delay, 1500);

    feed(&s, "5206");
    CHECK_EQ(r.media_open, 1);
    avdtp_src_media_connected(&s);
    CHECK_EQ(s.state, AVDTP_SRC_OPEN);
    CHECK_EQ(avdtp_src_start(&s), 0);
    expect_sent(&r, 7, "60070c");
    feed(&s, "6207");
    CHECK_EQ(s.state, AVDTP_SRC_STREAMING);
    CHECK_EQ(s.config.sample_rate, 96000);
}
