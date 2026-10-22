#include "avrcp.h"

#include <string.h>

/* AVCTP */
#define AVCTP_HDR 3
#define AVCTP_PID_AVRCP 0x110E
#define AVCTP_CMD 0
#define AVCTP_RSP 1

/* AV/C */
#define AVC_CTYPE_CONTROL 0x0
#define AVC_CTYPE_STATUS 0x1
#define AVC_CTYPE_NOTIFY 0x3
#define AVC_RSP_NOT_IMPLEMENTED 0x8
#define AVC_RSP_ACCEPTED 0x9
#define AVC_RSP_REJECTED 0xA
#define AVC_RSP_STABLE 0xC
#define AVC_RSP_CHANGED 0xD
#define AVC_RSP_INTERIM 0xF
#define AVC_SUBUNIT_PANEL 0x48 /* type 9 (panel) << 3 | id 0 */
#define AVC_SUBUNIT_UNIT 0xFF
#define AVC_OP_VENDOR 0x00
#define AVC_OP_UNIT_INFO 0x30
#define AVC_OP_SUBUNIT_INFO 0x31
#define AVC_OP_PASS_THROUGH 0x7C

/* AVRCP vendor-dependent PDUs (Bluetooth SIG company id 0x001958) */
#define PDU_GET_CAPABILITIES 0x10
#define PDU_INFORM_CHARSET 0x17
#define PDU_INFORM_BATTERY 0x18
#define PDU_GET_ELEMENT_ATTRIBUTES 0x20
#define PDU_GET_PLAY_STATUS 0x30
#define PDU_REGISTER_NOTIFICATION 0x31
#define PDU_SET_ABSOLUTE_VOLUME 0x50
#define CAP_COMPANY_ID 0x02
#define CAP_EVENTS 0x03
#define EVT_PLAYBACK_STATUS 0x01
#define EVT_TRACK_CHANGED 0x02
#define EVT_VOLUME_CHANGED 0x0D
#define ERR_INVALID_COMMAND 0x00
#define ERR_INVALID_PARAMETER 0x01
#define STATUS_PLAYING 0x01
#define STATUS_PAUSED 0x02

#define VENDOR_HDR 10 /* ctype, subunit, opcode, company[3], pdu, packet type, length[2] */
#define MAX_PACKET 64

static uint8_t new_label(avrcp *a)
{
    uint8_t l = a->next_label;
    a->next_label = (uint8_t)((a->next_label + 1) & 0x0F);
    return l;
}

static int send_raw(avrcp *a, const uint8_t *buf, size_t len)
{
    return a->ops && a->ops->send ? a->ops->send(a->ctx, buf, len) : -1;
}

static void avctp_header(uint8_t *p, uint8_t label, int cr)
{
    p[0] = (uint8_t)(label << 4 | (cr ? 0x02 : 0));
    p[1] = (uint8_t)(AVCTP_PID_AVRCP >> 8);
    p[2] = (uint8_t)(AVCTP_PID_AVRCP & 0xFF);
}

static int send_vendor(avrcp *a, uint8_t label, int cr, uint8_t ctype, uint8_t pdu, const uint8_t *params,
                       uint16_t plen)
{
    uint8_t p[MAX_PACKET];
    uint8_t *v = p + AVCTP_HDR;

    if ((size_t)AVCTP_HDR + VENDOR_HDR + plen > sizeof(p))
        return -1;
    avctp_header(p, label, cr);
    v[0] = ctype;
    v[1] = AVC_SUBUNIT_PANEL;
    v[2] = AVC_OP_VENDOR;
    v[3] = 0x00;
    v[4] = 0x19;
    v[5] = 0x58;
    v[6] = pdu;
    v[7] = 0; /* single packet */
    v[8] = (uint8_t)(plen >> 8);
    v[9] = (uint8_t)plen;
    if (plen)
        memcpy(v + VENDOR_HDR, params, plen);
    return send_raw(a, p, (size_t)AVCTP_HDR + VENDOR_HDR + plen);
}

/* Echo a command back as a response with another ctype (NOT_IMPLEMENTED, ACCEPTED). */
static int send_echo(avrcp *a, const uint8_t *buf, size_t len, uint8_t ctype)
{
    uint8_t p[MAX_PACKET];

    if (len > sizeof(p))
        len = sizeof(p);
    memcpy(p, buf, len);
    p[0] = (uint8_t)((p[0] & 0xF0) | 0x02);
    p[AVCTP_HDR] = ctype;
    return send_raw(a, p, len);
}

static int send_unit_info(avrcp *a, uint8_t label, uint8_t opcode)
{
    /* AV/C UNIT INFO / SUBUNIT INFO: one panel subunit, no company id. */
    uint8_t p[AVCTP_HDR + 8];

    avctp_header(p, label, AVCTP_RSP);
    p[3] = AVC_RSP_STABLE;
    p[4] = AVC_SUBUNIT_UNIT;
    p[5] = opcode;
    p[6] = 0x07;
    p[7] = AVC_SUBUNIT_PANEL;
    p[8] = 0xFF;
    p[9] = 0xFF;
    p[10] = 0xFF;
    return send_raw(a, p, sizeof(p));
}

static uint8_t play_status(const avrcp *a)
{
    return a->playing ? STATUS_PLAYING : STATUS_PAUSED;
}

static void register_volume(avrcp *a)
{
    static const uint8_t params[5] = { EVT_VOLUME_CHANGED, 0, 0, 0, 0 }; /* playback interval: unused */

    a->notify_label = new_label(a);
    a->notify_pending = send_vendor(a, a->notify_label, AVCTP_CMD, AVC_CTYPE_NOTIFY, PDU_REGISTER_NOTIFICATION,
                                    params, sizeof(params)) == 0;
}

static void send_set_volume(avrcp *a, uint8_t volume)
{
    a->set_label = new_label(a);
    a->set_pending = send_vendor(a, a->set_label, AVCTP_CMD, AVC_CTYPE_CONTROL, PDU_SET_ABSOLUTE_VOLUME, &volume, 1) == 0;
    a->want_volume = AVRCP_VOLUME_UNKNOWN;
}

void avrcp_init(avrcp *a, const avrcp_ops *ops, void *ctx)
{
    memset(a, 0, sizeof(*a));
    a->ops = ops;
    a->ctx = ctx;
    a->volume = AVRCP_VOLUME_UNKNOWN;
    a->want_volume = AVRCP_VOLUME_UNKNOWN;
}

void avrcp_connected(avrcp *a)
{
    static const uint8_t cap = CAP_EVENTS;
    uint8_t playing = a->playing;

    avrcp_init(a, a->ops, a->ctx);
    a->playing = playing;
    a->connected = 1;
    a->caps_label = new_label(a);
    a->caps_pending = send_vendor(a, a->caps_label, AVCTP_CMD, AVC_CTYPE_STATUS, PDU_GET_CAPABILITIES, &cap, 1) == 0;
}

void avrcp_disconnected(avrcp *a)
{
    uint8_t playing = a->playing;

    avrcp_init(a, a->ops, a->ctx);
    a->playing = playing;
}

/* ---- TG: commands from the headphones ---------------------------------------- */

static int on_vendor_command(avrcp *a, uint8_t label, uint8_t pdu, const uint8_t *params, uint16_t plen)
{
    uint8_t rsp[12];

    switch (pdu) {
    case PDU_GET_CAPABILITIES:
        if (plen >= 1 && params[0] == CAP_COMPANY_ID) {
            rsp[0] = CAP_COMPANY_ID;
            rsp[1] = 1;
            rsp[2] = 0x00;
            rsp[3] = 0x19;
            rsp[4] = 0x58;
            return send_vendor(a, label, AVCTP_RSP, AVC_RSP_STABLE, pdu, rsp, 5);
        }
        if (plen >= 1 && params[0] == CAP_EVENTS) {
            rsp[0] = CAP_EVENTS;
            rsp[1] = 2;
            rsp[2] = EVT_PLAYBACK_STATUS;
            rsp[3] = EVT_TRACK_CHANGED;
            return send_vendor(a, label, AVCTP_RSP, AVC_RSP_STABLE, pdu, rsp, 4);
        }
        rsp[0] = ERR_INVALID_PARAMETER;
        return send_vendor(a, label, AVCTP_RSP, AVC_RSP_REJECTED, pdu, rsp, 1);

    case PDU_REGISTER_NOTIFICATION:
        if (plen >= 1 && params[0] == EVT_PLAYBACK_STATUS) {
            a->status_registered = 1;
            a->status_label = label;
            rsp[0] = EVT_PLAYBACK_STATUS;
            rsp[1] = play_status(a);
            return send_vendor(a, label, AVCTP_RSP, AVC_RSP_INTERIM, pdu, rsp, 2);
        }
        if (plen >= 1 && params[0] == EVT_TRACK_CHANGED) {
            memset(rsp, 0, 9); /* track selected, no browsing: identifier 0 (as Windows answers) */
            rsp[0] = EVT_TRACK_CHANGED;
            return send_vendor(a, label, AVCTP_RSP, AVC_RSP_INTERIM, pdu, rsp, 9);
        }
        rsp[0] = ERR_INVALID_PARAMETER;
        return send_vendor(a, label, AVCTP_RSP, AVC_RSP_REJECTED, pdu, rsp, 1);

    case PDU_GET_ELEMENT_ATTRIBUTES:
        rsp[0] = 0; /* no attributes: no metadata */
        return send_vendor(a, label, AVCTP_RSP, AVC_RSP_STABLE, pdu, rsp, 1);

    case PDU_GET_PLAY_STATUS:
        memset(rsp, 0xFF, 8); /* song length and position not known */
        rsp[8] = play_status(a);
        return send_vendor(a, label, AVCTP_RSP, AVC_RSP_STABLE, pdu, rsp, 9);

    case PDU_INFORM_CHARSET:
    case PDU_INFORM_BATTERY:
        return send_vendor(a, label, AVCTP_RSP, AVC_RSP_ACCEPTED, pdu, NULL, 0);

    default:
        rsp[0] = ERR_INVALID_COMMAND;
        return send_vendor(a, label, AVCTP_RSP, AVC_RSP_REJECTED, pdu, rsp, 1);
    }
}

static int on_pass_through(avrcp *a, const uint8_t *buf, size_t len)
{
    const uint8_t *op = buf + AVCTP_HDR + 3;
    uint8_t id = op[0] & 0x7F;
    int pressed = !(op[0] & 0x80);

    switch (id) {
    case AVRCP_OP_VOLUME_UP:
    case AVRCP_OP_VOLUME_DOWN:
    case AVRCP_OP_MUTE:
    case AVRCP_OP_PLAY:
    case AVRCP_OP_STOP:
    case AVRCP_OP_PAUSE:
    case AVRCP_OP_REWIND:
    case AVRCP_OP_FAST_FORWARD:
    case AVRCP_OP_FORWARD:
    case AVRCP_OP_BACKWARD:
        send_echo(a, buf, len, AVC_RSP_ACCEPTED);
        if (a->ops->key)
            a->ops->key(a->ctx, id, pressed);
        return 0;
    default:
        return send_echo(a, buf, len, AVC_RSP_NOT_IMPLEMENTED) == 0 ? 0 : -1;
    }
}

/* ---- CT: responses from the headphones --------------------------------------- */

static void on_vendor_response(avrcp *a, uint8_t label, uint8_t ctype, uint8_t pdu, const uint8_t *params, uint16_t plen)
{
    uint16_t i;

    if (pdu == PDU_GET_CAPABILITIES && a->caps_pending && label == a->caps_label) {
        a->caps_pending = 0;
        a->abs_volume = AVRCP_ABSVOL_NO;
        if (ctype == AVC_RSP_STABLE && plen >= 2 && params[0] == CAP_EVENTS) {
            for (i = 0; i < params[1] && 2u + i < plen; i++)
                if (params[2 + i] == EVT_VOLUME_CHANGED)
                    a->abs_volume = AVRCP_ABSVOL_YES;
        }
        if (a->abs_volume == AVRCP_ABSVOL_YES)
            register_volume(a);
        return;
    }

    if (pdu == PDU_REGISTER_NOTIFICATION && a->notify_pending && label == a->notify_label && plen >= 1 &&
        params[0] == EVT_VOLUME_CHANGED) {
        if ((ctype == AVC_RSP_INTERIM || ctype == AVC_RSP_CHANGED) && plen >= 2) {
            a->volume = params[1] & AVRCP_VOLUME_MAX;
            if (a->ops->volume)
                a->ops->volume(a->ctx, a->volume);
            if (ctype == AVC_RSP_CHANGED)
                register_volume(a); /* notifications are one-shot */
        } else {
            a->notify_pending = 0; /* rejected: no volume reports */
        }
        return;
    }

    if (pdu == PDU_SET_ABSOLUTE_VOLUME && a->set_pending && label == a->set_label) {
        a->set_pending = 0;
        if (ctype == AVC_RSP_ACCEPTED && plen >= 1)
            a->volume = params[0] & AVRCP_VOLUME_MAX;
        if (a->want_volume != AVRCP_VOLUME_UNKNOWN && a->want_volume != a->volume)
            send_set_volume(a, a->want_volume);
        a->want_volume = AVRCP_VOLUME_UNKNOWN;
    }
}

/* ---- input ---------------------------------------------------------------------- */

int avrcp_input(avrcp *a, const uint8_t *buf, size_t len)
{
    const uint8_t *v;
    size_t vlen;
    uint8_t label, ctype, opcode;
    int cr;

    if (len < AVCTP_HDR)
        return -1;
    label = buf[0] >> 4;
    cr = (buf[0] >> 1) & 1;
    if ((buf[0] >> 2) & 3)
        return 0; /* fragmented: nothing in our subset */
    if (buf[0] & 1)
        return 0; /* the peer did not know our PID */
    if ((buf[1] << 8 | buf[2]) != AVCTP_PID_AVRCP) {
        if (cr == AVCTP_CMD) {
            uint8_t p[AVCTP_HDR] = { (uint8_t)(label << 4 | 0x03), buf[1], buf[2] }; /* IPID */
            send_raw(a, p, sizeof(p));
        }
        return 0;
    }

    v = buf + AVCTP_HDR;
    vlen = len - AVCTP_HDR;
    if (vlen < 3)
        return -1;
    ctype = v[0] & 0x0F;
    opcode = v[2];

    if (cr == AVCTP_RSP) {
        if (opcode == AVC_OP_VENDOR && vlen >= VENDOR_HDR) {
            uint16_t plen = (uint16_t)(v[8] << 8 | v[9]);
            if ((size_t)VENDOR_HDR + plen > vlen)
                return -1;
            on_vendor_response(a, label, ctype, v[6], v + VENDOR_HDR, plen);
        }
        return 0;
    }

    switch (opcode) {
    case AVC_OP_PASS_THROUGH:
        if (vlen < 5)
            return -1;
        return on_pass_through(a, buf, len);
    case AVC_OP_UNIT_INFO:
    case AVC_OP_SUBUNIT_INFO:
        return send_unit_info(a, label, opcode) == 0 ? 0 : -1;
    case AVC_OP_VENDOR:
        if (vlen >= VENDOR_HDR && v[3] == 0x00 && v[4] == 0x19 && v[5] == 0x58) {
            uint16_t plen = (uint16_t)(v[8] << 8 | v[9]);
            if ((size_t)VENDOR_HDR + plen > vlen)
                return -1;
            return on_vendor_command(a, label, v[6], v + VENDOR_HDR, plen) == 0 ? 0 : -1;
        }
        return send_echo(a, buf, len, AVC_RSP_NOT_IMPLEMENTED) == 0 ? 0 : -1;
    default:
        return send_echo(a, buf, len, AVC_RSP_NOT_IMPLEMENTED) == 0 ? 0 : -1;
    }
}

int avrcp_set_volume(avrcp *a, uint8_t volume)
{
    if (!a->connected || a->abs_volume != AVRCP_ABSVOL_YES)
        return -1;
    volume &= AVRCP_VOLUME_MAX;
    if (a->set_pending) {
        a->want_volume = volume; /* only the latest matters */
        return 0;
    }
    send_set_volume(a, volume);
    return a->set_pending ? 0 : -1;
}

void avrcp_set_playing(avrcp *a, int playing)
{
    uint8_t rsp[2];

    playing = playing != 0;
    if (a->playing == playing)
        return;
    a->playing = (uint8_t)playing;
    if (a->connected && a->status_registered) {
        a->status_registered = 0; /* one-shot: the headphones register again */
        rsp[0] = EVT_PLAYBACK_STATUS;
        rsp[1] = play_status(a);
        send_vendor(a, a->status_label, AVCTP_RSP, AVC_RSP_CHANGED, PDU_REGISTER_NOTIFICATION, rsp, 2);
    }
}

void avrcp_timeout(avrcp *a)
{
    if (a->caps_pending) {
        a->caps_pending = 0;
        a->abs_volume = AVRCP_ABSVOL_NO;
    }
    if (a->set_pending) {
        a->set_pending = 0;
        if (a->want_volume != AVRCP_VOLUME_UNKNOWN)
            send_set_volume(a, a->want_volume);
    }
}
