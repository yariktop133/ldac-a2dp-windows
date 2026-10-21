#include "avdtp_source.h"

#include <string.h>

static void set_state(avdtp_source *s, avdtp_src_state st)
{
    if (s->state == st)
        return;
    s->state = st;
    if (s->ops->state_changed)
        s->ops->state_changed(s->ctx, st);
}

static int send_raw(avdtp_source *s, uint8_t label, uint8_t msg_type, uint8_t signal,
                    const uint8_t *payload, size_t len)
{
    size_t n = avdtp_build(s->tx, sizeof(s->tx), label, msg_type, signal, payload, len);
    if (!n)
        return -1;
    return s->ops->send(s->ctx, s->tx, n);
}

static int send_cmd(avdtp_source *s, uint8_t signal, const uint8_t *payload, size_t len)
{
    uint8_t label = s->next_label;
    s->next_label = (uint8_t)((s->next_label + 1) & 0x0F);
    s->pend_active = 1;
    s->pend_label = label;
    s->pend_signal = signal;
    return send_raw(s, label, AVDTP_MSG_COMMAND, signal, payload, len);
}

static int send_seid_cmd(avdtp_source *s, uint8_t signal, uint8_t seid)
{
    uint8_t b = (uint8_t)(seid << 2);
    return send_cmd(s, signal, &b, 1);
}

static void reject(avdtp_source *s, const avdtp_msg *m, uint8_t err)
{
    send_raw(s, m->label, AVDTP_MSG_REJECT, m->signal, &err, 1);
}

/* SET_CONFIGURATION / RECONFIGURE rejects carry the service category first. */
static void reject_cat(avdtp_source *s, const avdtp_msg *m, uint8_t cat, uint8_t err)
{
    uint8_t p[2] = { cat, err };
    send_raw(s, m->label, AVDTP_MSG_REJECT, m->signal, p, 2);
}

/* START / SUSPEND rejects carry the failing ACP SEID first. */
static void reject_seid(avdtp_source *s, const avdtp_msg *m, uint8_t seid, uint8_t err)
{
    uint8_t p[2] = { (uint8_t)(seid << 2), err };
    send_raw(s, m->label, AVDTP_MSG_REJECT, m->signal, p, 2);
}

static void accept(avdtp_source *s, const avdtp_msg *m, const uint8_t *payload, size_t len)
{
    send_raw(s, m->label, AVDTP_MSG_ACCEPT, m->signal, payload, len);
}

static avdtp_local_sep *local_sep(avdtp_source *s, uint8_t seid)
{
    size_t i;
    for (i = 0; i < s->n_local; i++)
        if (s->local[i].seid == seid)
            return &s->local[i];
    return NULL;
}

static void reset_stream(avdtp_source *s)
{
    s->local_seid = 0;
    s->remote_seid = 0;
    s->peer_delay_reporting = 0;
    memset(&s->config, 0, sizeof(s->config));
}

static void build_local_seps(avdtp_source *s)
{
    a2dp_ldac ldac;
    a2dp_sbc sbc;
    avdtp_caps *c;

    s->n_local = 0;
    if (s->prefs.allow_ldac) {
        ldac.freq = s->prefs.ldac_freq_mask & (LDAC_FREQ_44100 | LDAC_FREQ_48000 | LDAC_FREQ_88200 | LDAC_FREQ_96000);
        ldac.cm = s->prefs.ldac_cm_mask & (LDAC_CM_STEREO | LDAC_CM_DUAL | LDAC_CM_MONO);
        if (ldac.freq && ldac.cm) {
            c = &s->local[s->n_local].caps;
            s->local[s->n_local].seid = AVDTP_SRC_SEID_LDAC;
            memset(c, 0, sizeof(*c));
            c->media_transport = 1;
            c->delay_reporting = 1;
            c->has_codec = 1;
            c->media_type = AVDTP_MEDIA_AUDIO;
            c->codec_type = A2DP_CODEC_VENDOR;
            c->codec_info_len = (uint8_t)a2dp_ldac_build(&ldac, c->codec_info, sizeof(c->codec_info));
            s->n_local++;
        }
    }

    sbc.freq = SBC_FREQ_44100 | SBC_FREQ_48000;
    sbc.cm = SBC_CM_MONO | SBC_CM_DUAL | SBC_CM_STEREO | SBC_CM_JOINT;
    sbc.block = SBC_BLOCK_4 | SBC_BLOCK_8 | SBC_BLOCK_12 | SBC_BLOCK_16;
    sbc.subbands = SBC_SUBBANDS_4 | SBC_SUBBANDS_8;
    sbc.alloc = SBC_ALLOC_SNR | SBC_ALLOC_LOUDNESS;
    sbc.min_bitpool = 2;
    sbc.max_bitpool = SBC_BITPOOL_HQ;
    c = &s->local[s->n_local].caps;
    s->local[s->n_local].seid = AVDTP_SRC_SEID_SBC;
    memset(c, 0, sizeof(*c));
    c->media_transport = 1;
    c->delay_reporting = 1;
    c->has_codec = 1;
    c->media_type = AVDTP_MEDIA_AUDIO;
    c->codec_type = A2DP_CODEC_SBC;
    c->codec_info_len = (uint8_t)a2dp_sbc_build(&sbc, c->codec_info, sizeof(c->codec_info));
    s->n_local++;
}

void avdtp_src_init(avdtp_source *s, const avdtp_src_ops *ops, void *ctx, const a2dp_prefs *prefs)
{
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->ctx = ctx;
    if (prefs)
        s->prefs = *prefs;
    else
        a2dp_prefs_default(&s->prefs);
    s->auto_start = 1;
    s->use_get_all = 1;
    build_local_seps(s);
}

int avdtp_src_connect(avdtp_source *s)
{
    if (s->state != AVDTP_SRC_IDLE && s->state != AVDTP_SRC_FAILED)
        return -1;
    s->n_remote = 0;
    s->ldac_rejected = 0;
    set_state(s, AVDTP_SRC_DISCOVERING);
    return send_cmd(s, AVDTP_DISCOVER, NULL, 0);
}

/* ---- initiator flow ------------------------------------------------------ */

static int configure_best(avdtp_source *s);

static int query_next(avdtp_source *s)
{
    if (s->query_idx < s->n_remote) {
        set_state(s, AVDTP_SRC_GETTING_CAPS);
        return send_seid_cmd(s, s->use_get_all ? AVDTP_GET_ALL_CAPABILITIES : AVDTP_GET_CAPABILITIES,
                             s->remote[s->query_idx].info.seid);
    }
    return configure_best(s);
}

static int send_set_configuration(avdtp_source *s, const avdtp_remote_sep *r, uint8_t local_seid,
                                  const a2dp_config *cfg)
{
    uint8_t p[2 + 2 + 2 + 4 + AVDTP_MAX_CODEC_INFO];
    avdtp_caps c;
    size_t n;

    memset(&c, 0, sizeof(c));
    c.media_transport = 1;
    c.delay_reporting = r->caps.delay_reporting;
    c.has_codec = 1;
    c.media_type = AVDTP_MEDIA_AUDIO;
    if (cfg->codec == A2DP_SEL_LDAC) {
        c.codec_type = A2DP_CODEC_VENDOR;
        c.codec_info_len = (uint8_t)a2dp_ldac_build(&cfg->ldac, c.codec_info, sizeof(c.codec_info));
    } else {
        c.codec_type = A2DP_CODEC_SBC;
        c.codec_info_len = (uint8_t)a2dp_sbc_build(&cfg->sbc, c.codec_info, sizeof(c.codec_info));
    }
    p[0] = (uint8_t)(r->info.seid << 2);
    p[1] = (uint8_t)(local_seid << 2);
    n = avdtp_caps_build(&c, 1, p + 2, sizeof(p) - 2);

    s->local_seid = local_seid;
    s->remote_seid = r->info.seid;
    s->peer_delay_reporting = r->caps.delay_reporting;
    s->config = *cfg;
    set_state(s, AVDTP_SRC_CONFIGURING);
    return send_cmd(s, AVDTP_SET_CONFIGURATION, p, n + 2);
}

static int configure_best(avdtp_source *s)
{
    size_t i;
    a2dp_config cfg;
    avdtp_local_sep *l;

    l = local_sep(s, AVDTP_SRC_SEID_LDAC);
    if (l && !s->ldac_rejected) {
        a2dp_ldac local, peer;
        a2dp_ldac_parse(l->caps.codec_type, l->caps.codec_info, l->caps.codec_info_len, &local);
        for (i = 0; i < s->n_remote; i++) {
            const avdtp_caps *c = &s->remote[i].caps;
            if (!s->remote[i].caps_valid || !c->has_codec ||
                a2dp_ldac_parse(c->codec_type, c->codec_info, c->codec_info_len, &peer) != 0)
                continue;
            if (a2dp_select_ldac(&local, &peer, s->prefs.ldac_pref_rate, &cfg) == 0)
                return send_set_configuration(s, &s->remote[i], l->seid, &cfg);
        }
    }

    l = local_sep(s, AVDTP_SRC_SEID_SBC);
    if (l) {
        a2dp_sbc local, peer;
        a2dp_sbc_parse(l->caps.codec_type, l->caps.codec_info, l->caps.codec_info_len, &local);
        for (i = 0; i < s->n_remote; i++) {
            const avdtp_caps *c = &s->remote[i].caps;
            if (!s->remote[i].caps_valid || !c->has_codec ||
                a2dp_sbc_parse(c->codec_type, c->codec_info, c->codec_info_len, &peer) != 0)
                continue;
            if (a2dp_select_sbc(&local, &peer, s->prefs.sbc_pref_rate, &cfg) == 0)
                return send_set_configuration(s, &s->remote[i], l->seid, &cfg);
        }
    }

    reset_stream(s);
    set_state(s, AVDTP_SRC_FAILED);
    return -1;
}

static void on_response(avdtp_source *s, const avdtp_msg *m)
{
    int ok = m->msg_type == AVDTP_MSG_ACCEPT;

    if (!s->pend_active || m->label != s->pend_label || m->signal != s->pend_signal)
        return; /* stale or unsolicited */
    s->pend_active = 0;

    switch (m->signal) {
    case AVDTP_DISCOVER: {
        avdtp_sep_info seps[AVDTP_MAX_SEPS];
        size_t n, i;
        if (!ok) {
            set_state(s, AVDTP_SRC_FAILED);
            return;
        }
        n = avdtp_discover_parse(m->payload, m->len, seps, AVDTP_MAX_SEPS);
        s->n_remote = 0;
        for (i = 0; i < n; i++) {
            if (seps[i].media_type != AVDTP_MEDIA_AUDIO || seps[i].tsep != AVDTP_TSEP_SNK || seps[i].in_use)
                continue;
            memset(&s->remote[s->n_remote], 0, sizeof(s->remote[0]));
            s->remote[s->n_remote++].info = seps[i];
        }
        if (s->n_remote == 0) {
            set_state(s, AVDTP_SRC_FAILED);
            return;
        }
        s->query_idx = 0;
        query_next(s);
        return;
    }

    case AVDTP_GET_ALL_CAPABILITIES:
    case AVDTP_GET_CAPABILITIES:
        if (ok) {
            avdtp_remote_sep *r = &s->remote[s->query_idx];
            r->caps_valid = avdtp_caps_parse(m->payload, m->len, &r->caps, NULL) == AVDTP_ERR_NONE;
            s->query_idx++;
        } else if (m->signal == AVDTP_GET_ALL_CAPABILITIES) {
            s->use_get_all = 0; /* AVDTP < 1.3 peer: retry the same SEP */
        } else {
            s->query_idx++;
        }
        query_next(s);
        return;

    case AVDTP_SET_CONFIGURATION:
        if (!ok) {
            if (s->config.codec == A2DP_SEL_LDAC) {
                s->ldac_rejected = 1;
                reset_stream(s);
                configure_best(s);
            } else {
                reset_stream(s);
                set_state(s, AVDTP_SRC_FAILED);
            }
            return;
        }
        set_state(s, AVDTP_SRC_CONFIGURED);
        if (s->ops->configured)
            s->ops->configured(s->ctx, &s->config);
        set_state(s, AVDTP_SRC_OPENING);
        send_seid_cmd(s, AVDTP_OPEN, s->remote_seid);
        return;

    case AVDTP_OPEN:
        if (!ok) {
            avdtp_src_abort(s);
            return;
        }
        set_state(s, AVDTP_SRC_WAIT_MEDIA);
        s->ops->media_open(s->ctx, 1);
        return;

    /* The peer may have started/suspended the stream itself while our
     * command was in flight; only roll back from the transitional state. */
    case AVDTP_START:
        if (s->state == AVDTP_SRC_STARTING)
            set_state(s, ok ? AVDTP_SRC_STREAMING : AVDTP_SRC_OPEN);
        return;

    case AVDTP_SUSPEND:
        if (s->state == AVDTP_SRC_SUSPENDING)
            set_state(s, ok ? AVDTP_SRC_OPEN : AVDTP_SRC_STREAMING);
        return;

    case AVDTP_CLOSE:
    case AVDTP_ABORT:
        if (s->ops->media_close)
            s->ops->media_close(s->ctx);
        reset_stream(s);
        set_state(s, AVDTP_SRC_IDLE);
        return;
    }
}

/* ---- acceptor role --------------------------------------------------------- */

static int stream_configured(const avdtp_source *s)
{
    return s->local_seid != 0 && s->state >= AVDTP_SRC_CONFIGURED && s->state <= AVDTP_SRC_SUSPENDING;
}

static void on_set_configuration(avdtp_source *s, const avdtp_msg *m)
{
    avdtp_local_sep *l;
    avdtp_caps caps;
    a2dp_config cfg;
    uint8_t bad_cat = 0;
    int err;

    memset(&cfg, 0, sizeof(cfg));
    if (m->len < 2) {
        reject_cat(s, m, 0, AVDTP_ERR_BAD_LENGTH);
        return;
    }
    l = local_sep(s, m->payload[0] >> 2);
    if (!l) {
        reject_cat(s, m, 0, AVDTP_ERR_BAD_ACP_SEID);
        return;
    }
    if (stream_configured(s)) {
        reject_cat(s, m, 0, AVDTP_ERR_SEP_IN_USE);
        return;
    }
    err = avdtp_caps_parse(m->payload + 2, m->len - 2, &caps, &bad_cat);
    if (err != AVDTP_ERR_NONE) {
        reject_cat(s, m, bad_cat, (uint8_t)err);
        return;
    }
    if (!caps.media_transport || !caps.has_codec || caps.codec_type != l->caps.codec_type) {
        reject_cat(s, m, AVDTP_CAT_MEDIA_CODEC, AVDTP_ERR_INVALID_CAPABILITIES);
        return;
    }
    if (caps.codec_type == A2DP_CODEC_VENDOR) {
        a2dp_ldac local, want;
        a2dp_ldac_parse(l->caps.codec_type, l->caps.codec_info, l->caps.codec_info_len, &local);
        err = a2dp_ldac_parse(caps.codec_type, caps.codec_info, caps.codec_info_len, &want) != 0
                  ? A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE
                  : a2dp_check_ldac_config(&local, &want, &cfg);
    } else {
        a2dp_sbc local, want;
        a2dp_sbc_parse(l->caps.codec_type, l->caps.codec_info, l->caps.codec_info_len, &local);
        err = a2dp_sbc_parse(caps.codec_type, caps.codec_info, caps.codec_info_len, &want) != 0
                  ? A2DP_ERR_INVALID_CODEC_TYPE
                  : a2dp_check_sbc_config(&local, &want, &cfg);
    }
    if (err) {
        reject_cat(s, m, AVDTP_CAT_MEDIA_CODEC, (uint8_t)err);
        return;
    }

    /* Peer wins any collision: drop our own in-flight discovery/config. */
    s->pend_active = 0;
    s->local_seid = l->seid;
    s->remote_seid = m->payload[1] >> 2;
    s->peer_delay_reporting = caps.delay_reporting;
    s->config = cfg;
    accept(s, m, NULL, 0);
    set_state(s, AVDTP_SRC_CONFIGURED);
    if (s->ops->configured)
        s->ops->configured(s->ctx, &s->config);
}

static void on_command(avdtp_source *s, const avdtp_msg *m)
{
    uint8_t buf[AVDTP_SIG_MTU_DEFAULT];
    uint8_t seid = m->len ? (uint8_t)(m->payload[0] >> 2) : 0;
    avdtp_local_sep *l;
    size_t n, i;

    switch (m->signal) {
    case AVDTP_DISCOVER: {
        avdtp_sep_info info[2];
        for (i = 0; i < s->n_local; i++) {
            info[i].seid = s->local[i].seid;
            info[i].in_use = stream_configured(s) && s->local_seid == s->local[i].seid;
            info[i].media_type = AVDTP_MEDIA_AUDIO;
            info[i].tsep = AVDTP_TSEP_SRC;
        }
        n = avdtp_discover_build(buf, sizeof(buf), info, s->n_local);
        accept(s, m, buf, n);
        return;
    }

    case AVDTP_GET_CAPABILITIES:
    case AVDTP_GET_ALL_CAPABILITIES:
        l = local_sep(s, seid);
        if (m->len < 1 || !l) {
            reject(s, m, m->len < 1 ? AVDTP_ERR_BAD_LENGTH : AVDTP_ERR_BAD_ACP_SEID);
            return;
        }
        n = avdtp_caps_build(&l->caps, m->signal == AVDTP_GET_ALL_CAPABILITIES, buf, sizeof(buf));
        accept(s, m, buf, n);
        return;

    case AVDTP_SET_CONFIGURATION:
        on_set_configuration(s, m);
        return;

    case AVDTP_GET_CONFIGURATION: {
        avdtp_caps c;
        if (!stream_configured(s) || seid != s->local_seid) {
            reject(s, m, AVDTP_ERR_BAD_ACP_SEID);
            return;
        }
        memset(&c, 0, sizeof(c));
        c.media_transport = 1;
        c.delay_reporting = s->peer_delay_reporting;
        c.has_codec = 1;
        c.media_type = AVDTP_MEDIA_AUDIO;
        if (s->config.codec == A2DP_SEL_LDAC) {
            c.codec_type = A2DP_CODEC_VENDOR;
            c.codec_info_len = (uint8_t)a2dp_ldac_build(&s->config.ldac, c.codec_info, sizeof(c.codec_info));
        } else {
            c.codec_type = A2DP_CODEC_SBC;
            c.codec_info_len = (uint8_t)a2dp_sbc_build(&s->config.sbc, c.codec_info, sizeof(c.codec_info));
        }
        n = avdtp_caps_build(&c, 1, buf, sizeof(buf));
        accept(s, m, buf, n);
        return;
    }

    case AVDTP_RECONFIGURE:
        reject_cat(s, m, AVDTP_CAT_MEDIA_CODEC, AVDTP_ERR_UNSUPPORTED_CONFIGURATION);
        return;

    case AVDTP_OPEN:
        if (seid != s->local_seid || s->state != AVDTP_SRC_CONFIGURED) {
            reject(s, m, seid != s->local_seid ? AVDTP_ERR_BAD_ACP_SEID : AVDTP_ERR_BAD_STATE);
            return;
        }
        accept(s, m, NULL, 0);
        set_state(s, AVDTP_SRC_WAIT_MEDIA);
        s->ops->media_open(s->ctx, 0);
        return;

    case AVDTP_START:
        if (seid != s->local_seid || (s->state != AVDTP_SRC_OPEN && s->state != AVDTP_SRC_STARTING)) {
            reject_seid(s, m, seid, seid != s->local_seid ? AVDTP_ERR_BAD_ACP_SEID : AVDTP_ERR_BAD_STATE);
            return;
        }
        accept(s, m, NULL, 0);
        set_state(s, AVDTP_SRC_STREAMING);
        return;

    case AVDTP_SUSPEND:
        if (seid != s->local_seid || s->state != AVDTP_SRC_STREAMING) {
            reject_seid(s, m, seid, seid != s->local_seid ? AVDTP_ERR_BAD_ACP_SEID : AVDTP_ERR_BAD_STATE);
            return;
        }
        accept(s, m, NULL, 0);
        set_state(s, AVDTP_SRC_OPEN);
        return;

    case AVDTP_CLOSE:
        if (seid != s->local_seid || s->state < AVDTP_SRC_WAIT_MEDIA || s->state > AVDTP_SRC_SUSPENDING) {
            reject(s, m, seid != s->local_seid ? AVDTP_ERR_BAD_ACP_SEID : AVDTP_ERR_BAD_STATE);
            return;
        }
        accept(s, m, NULL, 0);
        if (s->ops->media_close)
            s->ops->media_close(s->ctx);
        reset_stream(s);
        set_state(s, AVDTP_SRC_IDLE);
        return;

    case AVDTP_ABORT:
        accept(s, m, NULL, 0);
        if (s->ops->media_close)
            s->ops->media_close(s->ctx);
        s->pend_active = 0;
        reset_stream(s);
        set_state(s, AVDTP_SRC_IDLE);
        return;

    case AVDTP_DELAYREPORT:
        if (m->len < 3 || seid != s->local_seid) {
            reject(s, m, m->len < 3 ? AVDTP_ERR_BAD_LENGTH : AVDTP_ERR_BAD_ACP_SEID);
            return;
        }
        s->delay_100us = (uint16_t)(m->payload[1] << 8 | m->payload[2]);
        accept(s, m, NULL, 0);
        if (s->ops->delay_report)
            s->ops->delay_report(s->ctx, s->delay_100us);
        return;

    case AVDTP_SECURITY_CONTROL:
        reject(s, m, AVDTP_ERR_BAD_STATE);
        return;

    default:
        send_raw(s, m->label, AVDTP_MSG_GENERAL_REJECT, m->signal, NULL, 0);
        return;
    }
}

int avdtp_src_on_signal(avdtp_source *s, const uint8_t *pkt, size_t len)
{
    avdtp_msg m;
    int r = avdtp_reasm_push(&s->reasm, pkt, len, &m);
    if (r <= 0)
        return r;
    if (m.msg_type == AVDTP_MSG_COMMAND)
        on_command(s, &m);
    else
        on_response(s, &m);
    return 0;
}

void avdtp_src_media_connected(avdtp_source *s)
{
    if (s->state != AVDTP_SRC_WAIT_MEDIA)
        return;
    set_state(s, AVDTP_SRC_OPEN);
    if (s->auto_start)
        avdtp_src_start(s);
}

void avdtp_src_disconnected(avdtp_source *s)
{
    if (s->ops->media_close && s->state >= AVDTP_SRC_WAIT_MEDIA && s->state <= AVDTP_SRC_SUSPENDING)
        s->ops->media_close(s->ctx);
    s->pend_active = 0;
    s->reasm.active = 0;
    reset_stream(s);
    set_state(s, AVDTP_SRC_IDLE);
}

void avdtp_src_timeout(avdtp_source *s)
{
    if (!s->pend_active)
        return;
    s->pend_active = 0;
    if (s->pend_signal == AVDTP_ABORT || !s->local_seid) {
        reset_stream(s);
        set_state(s, s->pend_signal == AVDTP_ABORT ? AVDTP_SRC_IDLE : AVDTP_SRC_FAILED);
        return;
    }
    avdtp_src_abort(s);
}

int avdtp_src_start(avdtp_source *s)
{
    if (s->state != AVDTP_SRC_OPEN || s->pend_active)
        return -1;
    set_state(s, AVDTP_SRC_STARTING);
    return send_seid_cmd(s, AVDTP_START, s->remote_seid);
}

int avdtp_src_suspend(avdtp_source *s)
{
    if (s->state != AVDTP_SRC_STREAMING || s->pend_active)
        return -1;
    set_state(s, AVDTP_SRC_SUSPENDING);
    return send_seid_cmd(s, AVDTP_SUSPEND, s->remote_seid);
}

int avdtp_src_close(avdtp_source *s)
{
    if (s->state < AVDTP_SRC_WAIT_MEDIA || s->state > AVDTP_SRC_STREAMING || s->pend_active)
        return -1;
    set_state(s, AVDTP_SRC_CLOSING);
    return send_seid_cmd(s, AVDTP_CLOSE, s->remote_seid);
}

int avdtp_src_abort(avdtp_source *s)
{
    if (!s->remote_seid)
        return -1;
    set_state(s, AVDTP_SRC_ABORTING);
    return send_seid_cmd(s, AVDTP_ABORT, s->remote_seid);
}

const char *avdtp_src_state_name(avdtp_src_state st)
{
    static const char *const names[] = {
        "IDLE", "DISCOVERING", "GETTING_CAPS", "CONFIGURING", "CONFIGURED", "OPENING", "WAIT_MEDIA",
        "OPEN", "STARTING", "STREAMING", "SUSPENDING", "CLOSING", "ABORTING", "FAILED",
    };
    return (size_t)st < sizeof(names) / sizeof(names[0]) ? names[st] : "?";
}
