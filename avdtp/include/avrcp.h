/*
 * AVRCP 1.6 over AVCTP 1.4 for an A2DP source: just what headphones need.
 *
 * Controller (CT) towards the headphones: GetCapabilities, VOLUME_CHANGED
 * notifications and SetAbsoluteVolume (absolute volume: the volume lives in
 * the headphones, the PCM keeps its full resolution).
 * Target (TG) for the headphones: PASS THROUGH buttons (play, pause, next...),
 * PLAYBACK_STATUS / TRACK_CHANGED notifications, empty metadata, UNIT/SUBUNIT
 * INFO. The rest is answered NOT_IMPLEMENTED / REJECTED.
 *
 * Transport-agnostic like the AVDTP core: the owner feeds received AVCTP
 * packets to avrcp_input() and sends what ops->send gets on the same L2CAP
 * channel (PSM 0x17). Single-packet AVCTP only; fragments are ignored (nothing
 * in this subset needs them). See docs/jbl-live-780nc.md for the reference
 * exchange with JBL Live 780NC.
 */
#ifndef AVRCP_H
#define AVRCP_H

#include "avdtp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AVRCP_PSM 0x17
#define AVRCP_VOLUME_MAX 0x7F
#define AVRCP_VOLUME_UNKNOWN 0xFF

/* PASS THROUGH operation ids */
#define AVRCP_OP_VOLUME_UP 0x41
#define AVRCP_OP_VOLUME_DOWN 0x42
#define AVRCP_OP_MUTE 0x43
#define AVRCP_OP_PLAY 0x44
#define AVRCP_OP_STOP 0x45
#define AVRCP_OP_PAUSE 0x46
#define AVRCP_OP_REWIND 0x48
#define AVRCP_OP_FAST_FORWARD 0x49
#define AVRCP_OP_FORWARD 0x4B
#define AVRCP_OP_BACKWARD 0x4C

typedef struct avrcp_ops {
    /* Send one AVCTP packet. Return 0 on success. */
    int (*send)(void *ctx, const uint8_t *buf, size_t len);
    /* The headphones' volume (0..127): the current one once they confirm
     * absolute volume, then every change made on the headphones. */
    void (*volume)(void *ctx, uint8_t volume);
    /* A button on the headphones (PASS THROUGH), pressed or released. */
    void (*key)(void *ctx, uint8_t op, int pressed);
} avrcp_ops;

typedef enum avrcp_abs_volume {
    AVRCP_ABSVOL_UNKNOWN = 0, /* capabilities not known yet */
    AVRCP_ABSVOL_YES,
    AVRCP_ABSVOL_NO,
} avrcp_abs_volume;

typedef struct avrcp {
    const avrcp_ops *ops;
    void *ctx;
    uint8_t connected;
    uint8_t next_label;

    /* CT */
    avrcp_abs_volume abs_volume;
    uint8_t volume;           /* last known headphone volume, AVRCP_VOLUME_UNKNOWN until reported */
    uint8_t caps_pending, caps_label;
    uint8_t notify_pending, notify_label;
    uint8_t set_pending, set_label;
    uint8_t want_volume;      /* to send once the pending SetAbsoluteVolume is answered */

    /* TG */
    uint8_t playing;
    uint8_t status_registered, status_label;
} avrcp;

void avrcp_init(avrcp *a, const avrcp_ops *ops, void *ctx);
/* The AVCTP channel is up (either side opened it): asks for capabilities. */
void avrcp_connected(avrcp *a);
void avrcp_disconnected(avrcp *a);
/* One received AVCTP packet. Returns 0 if understood, -1 if malformed. */
int avrcp_input(avrcp *a, const uint8_t *buf, size_t len);
/* Absolute volume 0..127. Returns 0 if sent or queued, -1 if the headphones
 * have no absolute volume (or it is not known yet). */
int avrcp_set_volume(avrcp *a, uint8_t volume);
/* Local playback state for the headphones (PLAYBACK_STATUS notification). */
void avrcp_set_playing(avrcp *a, int playing);
/* No answer to our commands for a while (owner's timer, ~2 s): forget them. */
void avrcp_timeout(avrcp *a);

#ifdef __cplusplus
}
#endif

#endif /* AVRCP_H */
