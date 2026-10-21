#include "avdtp.h"
#include "avdtp_media.h"
#include "test.h"

#include <string.h>

static void test_single(void)
{
    uint8_t buf[16];
    uint8_t seid = 3 << 2;
    avdtp_reasm r;
    avdtp_msg m;
    size_t n = avdtp_build(buf, sizeof(buf), 5, AVDTP_MSG_COMMAND, AVDTP_GET_CAPABILITIES, &seid, 1);

    CHECK_EQ(n, 3);
    CHECK_EQ(buf[0], 0x50);
    CHECK_EQ(buf[1], 0x02);
    memset(&r, 0, sizeof(r));
    CHECK_EQ(avdtp_reasm_push(&r, buf, n, &m), 1);
    CHECK_EQ(m.label, 5);
    CHECK_EQ(m.signal, AVDTP_GET_CAPABILITIES);
    CHECK_EQ(m.len, 1);
    CHECK_EQ(m.payload[0] >> 2, 3);
    CHECK_EQ(avdtp_build(buf, 3, 0, 0, 1, buf, 2), 0);
}

static void test_fragmented(void)
{
    /* label 2, ACCEPT, GET_ALL_CAPABILITIES split into 3 packets */
    const uint8_t start[] = { 0x26, 3, 0x0C, 0x01, 0x00 };
    const uint8_t cont[] = { 0x2A, 0x07, 0x06, 0x00, 0x00 };
    const uint8_t end[] = { 0x2E, 0x11, 0x15, 0x02, 0xFA };
    avdtp_reasm r;
    avdtp_msg m;
    avdtp_caps caps;

    memset(&r, 0, sizeof(r));
    CHECK_EQ(avdtp_reasm_push(&r, start, sizeof(start), &m), 0);
    CHECK_EQ(avdtp_reasm_push(&r, cont, sizeof(cont), &m), 0);
    CHECK_EQ(avdtp_reasm_push(&r, end, sizeof(end), &m), 1);
    CHECK_EQ(m.label, 2);
    CHECK_EQ(m.msg_type, AVDTP_MSG_ACCEPT);
    CHECK_EQ(m.signal, AVDTP_GET_ALL_CAPABILITIES);
    CHECK_EQ(m.len, 10);
    CHECK_EQ(avdtp_caps_parse(m.payload, m.len, &caps, NULL), AVDTP_ERR_NONE);
    CHECK(caps.media_transport);
    CHECK(caps.has_codec);
    CHECK_EQ(caps.codec_type, 0x00);
    CHECK_EQ(caps.codec_info_len, 4);
    CHECK_EQ(caps.codec_info[3], 0xFA);

    /* END without START */
    memset(&r, 0, sizeof(r));
    CHECK_EQ(avdtp_reasm_push(&r, end, sizeof(end), &m), -1);
    /* fragment count mismatch */
    memset(&r, 0, sizeof(r));
    CHECK_EQ(avdtp_reasm_push(&r, start, sizeof(start), &m), 0);
    CHECK_EQ(avdtp_reasm_push(&r, end, sizeof(end), &m), -1);
}

static void test_discover(void)
{
    avdtp_sep_info in[2] = { { 1, 0, AVDTP_MEDIA_AUDIO, AVDTP_TSEP_SNK }, { 7, 1, AVDTP_MEDIA_AUDIO, AVDTP_TSEP_SRC } };
    avdtp_sep_info out[4];
    uint8_t buf[8];
    size_t n = avdtp_discover_build(buf, sizeof(buf), in, 2);

    CHECK_EQ(n, 4);
    CHECK_EQ(buf[0], 0x04);
    CHECK_EQ(buf[1], 0x08);
    CHECK_EQ(buf[2], 0x1E);
    CHECK_EQ(buf[3], 0x00);
    CHECK_EQ(avdtp_discover_parse(buf, n, out, 4), 2);
    CHECK_EQ(out[1].seid, 7);
    CHECK_EQ(out[1].in_use, 1);
    CHECK_EQ(out[0].tsep, AVDTP_TSEP_SNK);
}

static void test_caps_errors(void)
{
    const uint8_t bad_transport[] = { 0x01, 0x01, 0x00 };
    const uint8_t bad_cat[] = { 0x01, 0x00, 0x09, 0x00 };
    const uint8_t truncated[] = { 0x07, 0x06, 0x00, 0x00 };
    avdtp_caps c;
    uint8_t cat;

    CHECK_EQ(avdtp_caps_parse(bad_transport, sizeof(bad_transport), &c, &cat), AVDTP_ERR_BAD_MEDIA_TRANSPORT_FORMAT);
    CHECK_EQ(cat, AVDTP_CAT_MEDIA_TRANSPORT);
    CHECK_EQ(avdtp_caps_parse(bad_cat, sizeof(bad_cat), &c, &cat), AVDTP_ERR_BAD_SERV_CATEGORY);
    CHECK_EQ(cat, 0x09);
    CHECK_EQ(avdtp_caps_parse(truncated, sizeof(truncated), &c, &cat), AVDTP_ERR_BAD_LENGTH);
}

static void test_media(void)
{
    avdtp_media_stream s;
    uint8_t frames[4] = { 0xAA, 1, 2, 3 };
    uint8_t pkt[32];
    size_t n;

    avdtp_media_init(&s, 0x11223344);
    n = avdtp_media_packet(&s, pkt, sizeof(pkt), frames, sizeof(frames), 2, 256);
    CHECK_EQ(n, 17);
    CHECK_EQ(pkt[0], 0x80);
    CHECK_EQ(pkt[1], 96);
    CHECK_EQ(pkt[3], 0);
    CHECK_EQ(pkt[8], 0x11);
    CHECK_EQ(pkt[12], 2);
    CHECK_EQ(pkt[13], 0xAA);
    n = avdtp_media_packet(&s, pkt, sizeof(pkt), frames, sizeof(frames), 1, 128);
    CHECK_EQ(pkt[3], 1);
    CHECK_EQ(pkt[7], 0x00);
    CHECK_EQ(pkt[6], 0x01); /* timestamp 256 */
    CHECK_EQ(avdtp_media_packet(&s, pkt, 16, frames, sizeof(frames), 1, 128), 0);
}

void test_msg(void)
{
    test_single();
    test_fragmented();
    test_discover();
    test_caps_errors();
    test_media();
}
