#include "avdtp.h"

#include <string.h>

#define HDR0(label, pkt, msg) (uint8_t)(((label) & 0x0F) << 4 | ((pkt) & 0x03) << 2 | ((msg) & 0x03))

size_t avdtp_build(uint8_t *buf, size_t cap, uint8_t label, uint8_t msg_type,
                   uint8_t signal, const uint8_t *payload, size_t len)
{
    if (cap < 2 || len > cap - 2)
        return 0;
    buf[0] = HDR0(label, AVDTP_PKT_SINGLE, msg_type);
    buf[1] = signal & 0x3F;
    if (len)
        memcpy(buf + 2, payload, len);
    return len + 2;
}

int avdtp_reasm_push(avdtp_reasm *r, const uint8_t *pkt, size_t len, avdtp_msg *out)
{
    uint8_t label, pkt_type, msg_type;

    if (len < 1)
        return -1;
    label = pkt[0] >> 4;
    pkt_type = (pkt[0] >> 2) & 0x03;
    msg_type = pkt[0] & 0x03;

    switch (pkt_type) {
    case AVDTP_PKT_SINGLE:
        if (len < 2)
            return -1;
        r->active = 0;
        out->label = label;
        out->msg_type = msg_type;
        out->signal = pkt[1] & 0x3F;
        out->payload = pkt + 2;
        out->len = len - 2;
        return 1;

    case AVDTP_PKT_START:
        if (len < 3 || pkt[1] < 2)
            return -1;
        r->active = 1;
        r->label = label;
        r->msg_type = msg_type;
        r->nosp = pkt[1];
        r->signal = pkt[2] & 0x3F;
        r->got = 1;
        r->len = 0;
        if (len - 3 > sizeof(r->buf))
            return r->active = 0, -1;
        memcpy(r->buf, pkt + 3, len - 3);
        r->len = len - 3;
        return 0;

    case AVDTP_PKT_CONTINUE:
    case AVDTP_PKT_END:
        if (!r->active || label != r->label)
            return -1;
        if (len - 1 > sizeof(r->buf) - r->len)
            return r->active = 0, -1;
        memcpy(r->buf + r->len, pkt + 1, len - 1);
        r->len += len - 1;
        r->got++;
        if (pkt_type == AVDTP_PKT_CONTINUE) {
            if (r->got >= r->nosp)
                return r->active = 0, -1;
            return 0;
        }
        r->active = 0;
        if (r->got != r->nosp)
            return -1;
        out->label = r->label;
        out->msg_type = r->msg_type;
        out->signal = r->signal;
        out->payload = r->buf;
        out->len = r->len;
        return 1;
    }
    return -1;
}

size_t avdtp_discover_build(uint8_t *buf, size_t cap, const avdtp_sep_info *seps, size_t n)
{
    size_t i;
    if (n * 2 > cap)
        return 0;
    for (i = 0; i < n; i++) {
        buf[2 * i] = (uint8_t)((seps[i].seid & 0x3F) << 2 | (seps[i].in_use ? 0x02 : 0));
        buf[2 * i + 1] = (uint8_t)((seps[i].media_type & 0x0F) << 4 | (seps[i].tsep & 1) << 3);
    }
    return n * 2;
}

size_t avdtp_discover_parse(const uint8_t *p, size_t len, avdtp_sep_info *seps, size_t max)
{
    size_t n = 0;
    for (; len >= 2 && n < max; p += 2, len -= 2) {
        seps[n].seid = p[0] >> 2;
        seps[n].in_use = (p[0] >> 1) & 1;
        seps[n].media_type = p[1] >> 4;
        seps[n].tsep = (p[1] >> 3) & 1;
        if (seps[n].seid == 0)
            continue; /* 0x00 is forbidden */
        n++;
    }
    return n;
}

int avdtp_caps_parse(const uint8_t *p, size_t len, avdtp_caps *out, uint8_t *bad_cat)
{
    memset(out, 0, sizeof(*out));
    if (bad_cat)
        *bad_cat = 0;

    while (len > 0) {
        uint8_t cat, losc;
        if (len < 2)
            return AVDTP_ERR_BAD_LENGTH;
        cat = p[0];
        losc = p[1];
        if ((size_t)losc + 2 > len) {
            if (bad_cat)
                *bad_cat = cat;
            return AVDTP_ERR_BAD_LENGTH;
        }
        switch (cat) {
        case AVDTP_CAT_MEDIA_TRANSPORT:
            if (losc != 0) {
                if (bad_cat)
                    *bad_cat = cat;
                return AVDTP_ERR_BAD_MEDIA_TRANSPORT_FORMAT;
            }
            out->media_transport = 1;
            break;
        case AVDTP_CAT_DELAY_REPORTING:
            out->delay_reporting = 1;
            break;
        case AVDTP_CAT_CONTENT_PROTECTION:
            out->content_protection = 1;
            break;
        case AVDTP_CAT_MEDIA_CODEC:
            if (losc < 2 || losc - 2 > AVDTP_MAX_CODEC_INFO) {
                if (bad_cat)
                    *bad_cat = cat;
                return AVDTP_ERR_BAD_PAYLOAD_FORMAT;
            }
            out->has_codec = 1;
            out->media_type = p[2] >> 4;
            out->codec_type = p[3];
            out->codec_info_len = (uint8_t)(losc - 2);
            memcpy(out->codec_info, p + 4, out->codec_info_len);
            break;
        default:
            if (cat == 0 || cat > AVDTP_CAT_DELAY_REPORTING) {
                if (bad_cat)
                    *bad_cat = cat;
                return AVDTP_ERR_BAD_SERV_CATEGORY;
            }
            /* reporting, recovery, ROHC, multiplexing: recognized but unused */
            break;
        }
        p += 2 + losc;
        len -= 2 + (size_t)losc;
    }
    return AVDTP_ERR_NONE;
}

size_t avdtp_caps_build(const avdtp_caps *c, int with_delay_reporting, uint8_t *buf, size_t cap)
{
    size_t n = 0;

    if (c->media_transport) {
        if (cap - n < 2)
            return 0;
        buf[n++] = AVDTP_CAT_MEDIA_TRANSPORT;
        buf[n++] = 0;
    }
    if (c->has_codec) {
        if (cap - n < (size_t)4 + c->codec_info_len)
            return 0;
        buf[n++] = AVDTP_CAT_MEDIA_CODEC;
        buf[n++] = (uint8_t)(2 + c->codec_info_len);
        buf[n++] = (uint8_t)(c->media_type << 4);
        buf[n++] = c->codec_type;
        memcpy(buf + n, c->codec_info, c->codec_info_len);
        n += c->codec_info_len;
    }
    if (with_delay_reporting && c->delay_reporting) {
        if (cap - n < 2)
            return 0;
        buf[n++] = AVDTP_CAT_DELAY_REPORTING;
        buf[n++] = 0;
    }
    return n;
}

const char *avdtp_signal_name(uint8_t signal)
{
    static const char *const names[] = {
        "?", "DISCOVER", "GET_CAPABILITIES", "SET_CONFIGURATION", "GET_CONFIGURATION",
        "RECONFIGURE", "OPEN", "START", "CLOSE", "SUSPEND", "ABORT", "SECURITY_CONTROL",
        "GET_ALL_CAPABILITIES", "DELAYREPORT",
    };
    return signal < sizeof(names) / sizeof(names[0]) ? names[signal] : "?";
}
