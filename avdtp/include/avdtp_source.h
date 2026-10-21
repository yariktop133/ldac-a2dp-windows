/*
 * A2DP source stream endpoint state machine (AVDTP INT and ACP roles).
 *
 * Transport-agnostic: the owner delivers signaling SDUs via avdtp_src_on_signal()
 * and receives outgoing SDUs through ops->send. The same code runs in the
 * user-mode simulator and, later, in the KMDF driver on top of BRB L2CAP.
 *
 * Initiator flow:  DISCOVER -> GET_ALL_CAPABILITIES (fallback GET_CAPABILITIES)
 *                  -> SET_CONFIGURATION (LDAC preferred, SBC fallback) -> OPEN
 *                  -> [media L2CAP channel] -> START
 * Peer-initiated configuration (common with headphones) is accepted as ACP.
 */
#ifndef AVDTP_SOURCE_H
#define AVDTP_SOURCE_H

#include "a2dp_codec.h"
#include "avdtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum avdtp_src_state {
    AVDTP_SRC_IDLE = 0,
    AVDTP_SRC_DISCOVERING,
    AVDTP_SRC_GETTING_CAPS,
    AVDTP_SRC_CONFIGURING,
    AVDTP_SRC_CONFIGURED,
    AVDTP_SRC_OPENING,
    AVDTP_SRC_WAIT_MEDIA,
    AVDTP_SRC_OPEN,
    AVDTP_SRC_STARTING,
    AVDTP_SRC_STREAMING,
    AVDTP_SRC_SUSPENDING,
    AVDTP_SRC_CLOSING,
    AVDTP_SRC_ABORTING,
    AVDTP_SRC_FAILED,
} avdtp_src_state;

typedef struct avdtp_src_ops {
    /* Send one signaling SDU. Return 0 on success. */
    int (*send)(void *ctx, const uint8_t *buf, size_t len);
    /* initiate=1: open the media L2CAP channel to PSM 0x19 now;
     * initiate=0: the peer will open it, accept the incoming connection.
     * Call avdtp_src_media_connected() once the channel is up. */
    void (*media_open)(void *ctx, int initiate);
    void (*media_close)(void *ctx);
    void (*state_changed)(void *ctx, avdtp_src_state state);
    void (*configured)(void *ctx, const a2dp_config *cfg);
    void (*delay_report)(void *ctx, uint16_t delay_100us);
} avdtp_src_ops;

typedef struct avdtp_remote_sep {
    avdtp_sep_info info;
    avdtp_caps caps;
    uint8_t caps_valid;
} avdtp_remote_sep;

#define AVDTP_SRC_SEID_LDAC 1
#define AVDTP_SRC_SEID_SBC 2

typedef struct avdtp_local_sep {
    uint8_t seid;
    avdtp_caps caps;
} avdtp_local_sep;

typedef struct avdtp_source {
    const avdtp_src_ops *ops;
    void *ctx;
    a2dp_prefs prefs;
    avdtp_src_state state;
    uint8_t auto_start;

    uint8_t next_label;
    uint8_t pend_active;
    uint8_t pend_label;
    uint8_t pend_signal;
    avdtp_reasm reasm;

    avdtp_local_sep local[2];
    size_t n_local;

    avdtp_remote_sep remote[AVDTP_MAX_SEPS];
    size_t n_remote;
    size_t query_idx;
    uint8_t use_get_all;
    uint8_t ldac_rejected;

    uint8_t local_seid;  /* configured stream, 0 if none */
    uint8_t remote_seid;
    uint8_t peer_delay_reporting;
    a2dp_config config;
    uint16_t delay_100us;

    uint8_t tx[AVDTP_SIG_MTU_DEFAULT];
} avdtp_source;

void avdtp_src_init(avdtp_source *s, const avdtp_src_ops *ops, void *ctx, const a2dp_prefs *prefs);

/* Signaling channel is up and we are the initiator: starts DISCOVER. */
int avdtp_src_connect(avdtp_source *s);
/* Incoming signaling SDU. Returns 0, or -1 on a malformed packet. */
int avdtp_src_on_signal(avdtp_source *s, const uint8_t *pkt, size_t len);
/* Media transport channel is established. */
void avdtp_src_media_connected(avdtp_source *s);
/* Signaling channel went down. */
void avdtp_src_disconnected(avdtp_source *s);
/* The pending command got no response in time (spec: RTX_SIG_TIMER). */
void avdtp_src_timeout(avdtp_source *s);

int avdtp_src_start(avdtp_source *s);
int avdtp_src_suspend(avdtp_source *s);
int avdtp_src_close(avdtp_source *s);
int avdtp_src_abort(avdtp_source *s);

const char *avdtp_src_state_name(avdtp_src_state st);

#ifdef __cplusplus
}
#endif

#endif /* AVDTP_SOURCE_H */
