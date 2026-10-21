/*
 * Source <-> simulated sink over in-memory "L2CAP" queues.
 * This is the stage-3 simulator: full signaling without radio or driver.
 */
#include "avdtp_source.h"
#include "sim_sink.h"
#include "test.h"

#include <string.h>

#define QCAP 64

typedef struct pkt {
    uint8_t data[AVDTP_SIG_MTU_DEFAULT];
    size_t len;
} pkt;

typedef struct queue {
    pkt items[QCAP];
    size_t head, tail;
} queue;

typedef struct harness {
    avdtp_source src;
    sim_sink sink;
    queue to_src, to_sink;
    int drop_to_sink;
    int media_open_req;
    int media_initiated;
    int media_closed;
    int configured_calls;
    uint16_t delay;
    int verbose;
} harness;

static void q_push(queue *q, const uint8_t *buf, size_t len)
{
    pkt *p = &q->items[q->tail++ % QCAP];
    memcpy(p->data, buf, len);
    p->len = len;
}

static int src_send(void *ctx, const uint8_t *buf, size_t len)
{
    harness *h = ctx;
    if (!h->drop_to_sink)
        q_push(&h->to_sink, buf, len);
    return 0;
}

static int sink_send(void *ctx, const uint8_t *buf, size_t len)
{
    harness *h = ctx;
    q_push(&h->to_src, buf, len);
    return 0;
}

static void media_open(void *ctx, int initiate)
{
    harness *h = ctx;
    h->media_open_req = 1;
    h->media_initiated = initiate;
}

static void media_close(void *ctx)
{
    ((harness *)ctx)->media_closed++;
}

static void configured(void *ctx, const a2dp_config *cfg)
{
    (void)cfg;
    ((harness *)ctx)->configured_calls++;
}

static void delay_report(void *ctx, uint16_t d)
{
    ((harness *)ctx)->delay = d;
}

static const avdtp_src_ops ops = { src_send, media_open, media_close, NULL, configured, delay_report };

static void setup(harness *h, const a2dp_prefs *prefs)
{
    memset(h, 0, sizeof(*h));
    avdtp_src_init(&h->src, &ops, h, prefs);
    sim_sink_init(&h->sink, sink_send, h);
}

static void pump(harness *h)
{
    int guard = 0;
    while (guard++ < 1000) {
        int moved = 0;
        while (h->to_sink.head != h->to_sink.tail) {
            pkt *p = &h->to_sink.items[h->to_sink.head++ % QCAP];
            sim_sink_on_signal(&h->sink, p->data, p->len);
            moved = 1;
        }
        while (h->to_src.head != h->to_src.tail) {
            pkt *p = &h->to_src.items[h->to_src.head++ % QCAP];
            CHECK_EQ(avdtp_src_on_signal(&h->src, p->data, p->len), 0);
            moved = 1;
        }
        if (h->media_open_req) {
            h->media_open_req = 0;
            avdtp_src_media_connected(&h->src);
            moved = 1;
        }
        if (!moved)
            return;
    }
    CHECK(!"pump did not settle");
}

static void test_ldac_happy_path(void)
{
    harness h;
    setup(&h, NULL);
    h.sink.delay_report = 1;
    CHECK_EQ(avdtp_src_connect(&h.src), 0);
    pump(&h);
    CHECK_EQ(h.src.state, AVDTP_SRC_STREAMING);
    CHECK_EQ(h.src.config.codec, A2DP_SEL_LDAC);
    CHECK_EQ(h.src.config.sample_rate, 96000);
    CHECK_EQ(h.src.remote_seid, SIM_SEID_LDAC);
    CHECK_EQ(h.sink.config.codec, A2DP_SEL_LDAC);
    CHECK_EQ(h.sink.config.sample_rate, 96000);
    CHECK_EQ(h.sink.remote_seid, AVDTP_SRC_SEID_LDAC);
    CHECK(h.sink.streaming);
    CHECK_EQ(h.media_initiated, 1);
    CHECK_EQ(h.configured_calls, 1);
    CHECK_EQ(h.delay, 1500);

    CHECK_EQ(avdtp_src_suspend(&h.src), 0);
    pump(&h);
    CHECK_EQ(h.src.state, AVDTP_SRC_OPEN);
    CHECK(!h.sink.streaming);
    CHECK_EQ(avdtp_src_start(&h.src), 0);
    pump(&h);
    CHECK_EQ(h.src.state, AVDTP_SRC_STREAMING);
    CHECK_EQ(avdtp_src_close(&h.src), 0);
    pump(&h);
    CHECK_EQ(h.src.state, AVDTP_SRC_IDLE);
    CHECK_EQ(h.media_closed, 1);
}

static void test_pref_rate(void)
{
    harness h;
    a2dp_prefs p;
    a2dp_prefs_default(&p);
    p.ldac_pref_rate = 48000;
    setup(&h, &p);
    avdtp_src_connect(&h.src);
    pump(&h);
    CHECK_EQ(h.src.config.codec, A2DP_SEL_LDAC);
    CHECK_EQ(h.src.config.sample_rate, 48000);

    /* rate restricted by the settings mask, as the UI checkboxes do */
    a2dp_prefs_default(&p);
    p.ldac_freq_mask = LDAC_FREQ_44100;
    setup(&h, &p);
    avdtp_src_connect(&h.src);
    pump(&h);
    CHECK_EQ(h.src.config.sample_rate, 44100);
}

static void test_avdtp_12_peer(void)
{
    harness h;
    setup(&h, NULL);
    h.sink.no_get_all = 1;
    avdtp_src_connect(&h.src);
    pump(&h);
    CHECK_EQ(h.src.state, AVDTP_SRC_STREAMING);
    CHECK_EQ(h.src.config.codec, A2DP_SEL_LDAC);
    CHECK_EQ(h.src.use_get_all, 0);
}

static void test_ldac_rejected_falls_back_to_sbc(void)
{
    harness h;
    setup(&h, NULL);
    h.sink.reject_ldac = 1;
    avdtp_src_connect(&h.src);
    pump(&h);
    CHECK_EQ(h.src.state, AVDTP_SRC_STREAMING);
    CHECK_EQ(h.src.config.codec, A2DP_SEL_SBC);
    CHECK_EQ(h.sink.configured_seid, SIM_SEID_SBC);
}

static void test_sbc_only_sink(void)
{
    harness h;
    setup(&h, NULL);
    h.sink.has_ldac = 0;
    avdtp_src_connect(&h.src);
    pump(&h);
    CHECK_EQ(h.src.state, AVDTP_SRC_STREAMING);
    CHECK_EQ(h.src.config.codec, A2DP_SEL_SBC);
    CHECK_EQ(h.src.config.sample_rate, 48000);
    CHECK_EQ(h.src.config.sbc.max_bitpool, 53);
}

static void test_ldac_disabled(void)
{
    harness h;
    a2dp_prefs p;
    a2dp_prefs_default(&p);
    p.allow_ldac = 0;
    setup(&h, &p);
    avdtp_src_connect(&h.src);
    pump(&h);
    CHECK_EQ(h.src.config.codec, A2DP_SEL_SBC);
    CHECK_EQ(h.src.n_local, 1);
}

static void test_sink_initiated(void)
{
    harness h;
    setup(&h, NULL);
    h.sink.initiate = 1;
    h.sink.delay_report = 1;
    h.sink.delay_100us = 2200;
    sim_sink_connect(&h.sink);
    pump(&h);
    CHECK_EQ(h.src.state, AVDTP_SRC_STREAMING);
    CHECK_EQ(h.src.config.codec, A2DP_SEL_LDAC);
    CHECK_EQ(h.src.config.sample_rate, 48000);
    CHECK_EQ(h.media_initiated, 0);
    CHECK_EQ(h.delay, 2200);
    CHECK(h.sink.streaming);
}

static void test_timeout(void)
{
    harness h;
    setup(&h, NULL);
    h.drop_to_sink = 1;
    avdtp_src_connect(&h.src);
    pump(&h);
    CHECK_EQ(h.src.state, AVDTP_SRC_DISCOVERING);
    avdtp_src_timeout(&h.src);
    CHECK_EQ(h.src.state, AVDTP_SRC_FAILED);
    /* reconnect after failure works */
    h.drop_to_sink = 0;
    CHECK_EQ(avdtp_src_connect(&h.src), 0);
    pump(&h);
    CHECK_EQ(h.src.state, AVDTP_SRC_STREAMING);
}

static void test_unknown_and_bad_commands(void)
{
    harness h;
    uint8_t buf[8];
    uint8_t seid;
    size_t n;
    setup(&h, NULL);

    /* unknown signal 0x3F -> GENERAL_REJECT */
    n = avdtp_build(buf, sizeof(buf), 9, AVDTP_MSG_COMMAND, 0x3F, NULL, 0);
    avdtp_src_on_signal(&h.src, buf, n);
    CHECK_EQ(h.to_sink.tail, 1);
    CHECK_EQ(h.to_sink.items[0].data[0], 0x91);
    CHECK_EQ(h.to_sink.items[0].data[1], 0x3F);

    /* GET_CAPABILITIES for a SEID we don't have -> REJECT BAD_ACP_SEID */
    seid = 9 << 2;
    n = avdtp_build(buf, sizeof(buf), 1, AVDTP_MSG_COMMAND, AVDTP_GET_CAPABILITIES, &seid, 1);
    avdtp_src_on_signal(&h.src, buf, n);
    CHECK_EQ(h.to_sink.items[1].data[0] & 3, AVDTP_MSG_REJECT);
    CHECK_EQ(h.to_sink.items[1].data[2], AVDTP_ERR_BAD_ACP_SEID);

    /* OPEN before configuration -> BAD_ACP_SEID / BAD_STATE */
    seid = AVDTP_SRC_SEID_LDAC << 2;
    n = avdtp_build(buf, sizeof(buf), 2, AVDTP_MSG_COMMAND, AVDTP_OPEN, &seid, 1);
    avdtp_src_on_signal(&h.src, buf, n);
    CHECK_EQ(h.to_sink.items[2].data[0] & 3, AVDTP_MSG_REJECT);
}

void test_loopback(void)
{
    test_ldac_happy_path();
    test_pref_rate();
    test_avdtp_12_peer();
    test_ldac_rejected_falls_back_to_sbc();
    test_sbc_only_sink();
    test_ldac_disabled();
    test_sink_initiated();
    test_timeout();
    test_unknown_and_bad_commands();
}
