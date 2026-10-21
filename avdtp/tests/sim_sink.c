#include "sim_sink.h"

#include <string.h>

void sim_sink_init(sim_sink *k, int (*send)(void *, const uint8_t *, size_t), void *ctx)
{
    memset(k, 0, sizeof(*k));
    k->has_ldac = 1;
    k->ldac.freq = LDAC_FREQ_44100 | LDAC_FREQ_48000 | LDAC_FREQ_88200 | LDAC_FREQ_96000;
    k->ldac.cm = LDAC_CM_STEREO | LDAC_CM_DUAL | LDAC_CM_MONO;
    k->delay_report = 1;
    k->delay_100us = 1500; /* 150 ms */
    k->send = send;
    k->ctx = ctx;
}

static void out(sim_sink *k, uint8_t label, uint8_t type, uint8_t sig, const uint8_t *p, size_t n)
{
    uint8_t buf[AVDTP_SIG_MTU_DEFAULT];
    size_t len = avdtp_build(buf, sizeof(buf), label, type, sig, p, n);
    k->send(k->ctx, buf, len);
}

static void cmd(sim_sink *k, uint8_t sig, const uint8_t *p, size_t n)
{
    out(k, k->label, AVDTP_MSG_COMMAND, sig, p, n);
    k->label = (uint8_t)((k->label + 1) & 0x0F);
}

static int sep_caps(const sim_sink *k, uint8_t seid, avdtp_caps *c)
{
    memset(c, 0, sizeof(*c));
    c->media_transport = 1;
    c->delay_reporting = 1;
    c->has_codec = 1;
    c->media_type = AVDTP_MEDIA_AUDIO;
    if (seid == SIM_SEID_LDAC && k->has_ldac) {
        c->codec_type = A2DP_CODEC_VENDOR;
        c->codec_info_len = (uint8_t)a2dp_ldac_build(&k->ldac, c->codec_info, sizeof(c->codec_info));
        return 0;
    }
    if (seid == SIM_SEID_SBC) {
        /* JBL: 44.1/48 kHz, all modes, blocks, subbands, allocations, bitpool 2..53 */
        a2dp_sbc sbc = { SBC_FREQ_44100 | SBC_FREQ_48000, 0x0F, 0xF0, 0x0C, 0x03, 2, 53 };
        c->codec_type = A2DP_CODEC_SBC;
        c->codec_info_len = (uint8_t)a2dp_sbc_build(&sbc, c->codec_info, sizeof(c->codec_info));
        return 0;
    }
    if (seid == SIM_SEID_AAC) {
        /* JBL AAC capabilities verbatim; the source has no AAC SEP and must skip it */
        static const uint8_t aac[] = { 0x80, 0x01, 0x8C, 0x84, 0x00, 0x00 };
        c->codec_type = 0x02;
        c->codec_info_len = sizeof(aac);
        memcpy(c->codec_info, aac, sizeof(aac));
        return 0;
    }
    return -1;
}

static void send_set_config(sim_sink *k, uint8_t src_seid, const avdtp_caps *src_caps)
{
    uint8_t p[64];
    avdtp_caps c;
    a2dp_ldac ldac;
    size_t n;

    memset(&c, 0, sizeof(c));
    c.media_transport = 1;
    c.delay_reporting = 1;
    c.has_codec = 1;
    if (a2dp_ldac_parse(src_caps->codec_type, src_caps->codec_info, src_caps->codec_info_len, &ldac) == 0 &&
        k->has_ldac) {
        a2dp_ldac cfg = { LDAC_FREQ_48000, LDAC_CM_STEREO };
        c.codec_type = A2DP_CODEC_VENDOR;
        c.codec_info_len = (uint8_t)a2dp_ldac_build(&cfg, c.codec_info, sizeof(c.codec_info));
        k->configured_seid = SIM_SEID_LDAC;
    } else {
        a2dp_sbc cfg = { SBC_FREQ_44100, SBC_CM_JOINT, SBC_BLOCK_16, SBC_SUBBANDS_8, SBC_ALLOC_LOUDNESS, 2, 53 };
        c.codec_type = A2DP_CODEC_SBC;
        c.codec_info_len = (uint8_t)a2dp_sbc_build(&cfg, c.codec_info, sizeof(c.codec_info));
        k->configured_seid = SIM_SEID_SBC;
    }
    p[0] = (uint8_t)(src_seid << 2);
    p[1] = (uint8_t)(k->configured_seid << 2);
    n = avdtp_caps_build(&c, 1, p + 2, sizeof(p) - 2);
    k->remote_seid = src_seid;
    cmd(k, AVDTP_SET_CONFIGURATION, p, n + 2);
}

void sim_sink_connect(sim_sink *k)
{
    k->init_step = 1;
    cmd(k, AVDTP_DISCOVER, NULL, 0);
}

static void on_response(sim_sink *k, const avdtp_msg *m)
{
    uint8_t b;
    if (!k->initiate || m->msg_type != AVDTP_MSG_ACCEPT)
        return;
    switch (m->signal) {
    case AVDTP_DISCOVER: {
        avdtp_sep_info seps[AVDTP_MAX_SEPS];
        size_t n = avdtp_discover_parse(m->payload, m->len, seps, AVDTP_MAX_SEPS);
        if (n == 0)
            return;
        b = (uint8_t)(seps[0].seid << 2); /* first SEP the source lists */
        cmd(k, AVDTP_GET_ALL_CAPABILITIES, &b, 1);
        k->remote_seid = seps[0].seid;
        return;
    }
    case AVDTP_GET_ALL_CAPABILITIES: {
        avdtp_caps c;
        if (avdtp_caps_parse(m->payload, m->len, &c, NULL) == AVDTP_ERR_NONE)
            send_set_config(k, k->remote_seid, &c);
        return;
    }
    case AVDTP_SET_CONFIGURATION:
        if (k->delay_report) {
            uint8_t d[3] = { (uint8_t)(k->remote_seid << 2), (uint8_t)(k->delay_100us >> 8), (uint8_t)k->delay_100us };
            cmd(k, AVDTP_DELAYREPORT, d, 3);
            return;
        }
        /* fall through */
    case AVDTP_DELAYREPORT:
        b = (uint8_t)(k->remote_seid << 2);
        cmd(k, AVDTP_OPEN, &b, 1);
        return;
    case AVDTP_OPEN:
        k->opened = 1;
        return;
    }
}

static void on_command(sim_sink *k, const avdtp_msg *m)
{
    uint8_t buf[128];
    uint8_t seid = m->len ? (uint8_t)(m->payload[0] >> 2) : 0;
    avdtp_caps c;
    size_t n;

    switch (m->signal) {
    case AVDTP_DISCOVER: {
        avdtp_sep_info seps[3];
        size_t cnt = 0;
        seps[cnt++] = (avdtp_sep_info){ SIM_SEID_SBC, 0, AVDTP_MEDIA_AUDIO, AVDTP_TSEP_SNK };
        seps[cnt++] = (avdtp_sep_info){ SIM_SEID_AAC, 0, AVDTP_MEDIA_AUDIO, AVDTP_TSEP_SNK };
        if (k->has_ldac)
            seps[cnt++] = (avdtp_sep_info){ SIM_SEID_LDAC, 0, AVDTP_MEDIA_AUDIO, AVDTP_TSEP_SNK };
        n = avdtp_discover_build(buf, sizeof(buf), seps, cnt);
        out(k, m->label, AVDTP_MSG_ACCEPT, m->signal, buf, n);
        return;
    }
    case AVDTP_GET_ALL_CAPABILITIES:
    case AVDTP_GET_CAPABILITIES:
        if (m->signal == AVDTP_GET_ALL_CAPABILITIES && k->no_get_all) {
            out(k, m->label, AVDTP_MSG_GENERAL_REJECT, m->signal, NULL, 0);
            return;
        }
        if (sep_caps(k, seid, &c) != 0) {
            buf[0] = AVDTP_ERR_BAD_ACP_SEID;
            out(k, m->label, AVDTP_MSG_REJECT, m->signal, buf, 1);
            return;
        }
        n = avdtp_caps_build(&c, m->signal == AVDTP_GET_ALL_CAPABILITIES, buf, sizeof(buf));
        out(k, m->label, AVDTP_MSG_ACCEPT, m->signal, buf, n);
        return;

    case AVDTP_SET_CONFIGURATION: {
        avdtp_caps want;
        if (m->len < 2 || avdtp_caps_parse(m->payload + 2, m->len - 2, &want, NULL) != AVDTP_ERR_NONE ||
            (seid == SIM_SEID_LDAC && k->reject_ldac)) {
            buf[0] = AVDTP_CAT_MEDIA_CODEC;
            buf[1] = A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE;
            out(k, m->label, AVDTP_MSG_REJECT, m->signal, buf, 2);
            return;
        }
        k->configured_seid = seid;
        k->remote_seid = m->payload[1] >> 2;
        memset(&k->config, 0, sizeof(k->config));
        if (a2dp_ldac_parse(want.codec_type, want.codec_info, want.codec_info_len, &k->config.ldac) == 0) {
            k->config.codec = A2DP_SEL_LDAC;
            k->config.sample_rate = a2dp_ldac_freq_to_rate(k->config.ldac.freq);
        } else if (a2dp_sbc_parse(want.codec_type, want.codec_info, want.codec_info_len, &k->config.sbc) == 0) {
            k->config.codec = A2DP_SEL_SBC;
        }
        out(k, m->label, AVDTP_MSG_ACCEPT, m->signal, NULL, 0);
        if (k->delay_report) {
            buf[0] = (uint8_t)(k->remote_seid << 2);
            buf[1] = (uint8_t)(k->delay_100us >> 8);
            buf[2] = (uint8_t)k->delay_100us;
            cmd(k, AVDTP_DELAYREPORT, buf, 3);
        }
        return;
    }
    case AVDTP_OPEN:
        k->opened = 1;
        out(k, m->label, AVDTP_MSG_ACCEPT, m->signal, NULL, 0);
        return;
    case AVDTP_START:
        k->streaming = 1;
        out(k, m->label, AVDTP_MSG_ACCEPT, m->signal, NULL, 0);
        return;
    case AVDTP_SUSPEND:
        k->streaming = 0;
        out(k, m->label, AVDTP_MSG_ACCEPT, m->signal, NULL, 0);
        return;
    case AVDTP_CLOSE:
    case AVDTP_ABORT:
        k->opened = 0;
        k->streaming = 0;
        k->configured_seid = 0;
        out(k, m->label, AVDTP_MSG_ACCEPT, m->signal, NULL, 0);
        return;
    default:
        out(k, m->label, AVDTP_MSG_GENERAL_REJECT, m->signal, NULL, 0);
        return;
    }
}

void sim_sink_on_signal(sim_sink *k, const uint8_t *pkt, size_t len)
{
    avdtp_msg m;
    if (avdtp_reasm_push(&k->reasm, pkt, len, &m) != 1)
        return;
    if (m.msg_type == AVDTP_MSG_COMMAND)
        on_command(k, &m);
    else
        on_response(k, &m);
}
