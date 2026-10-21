/*
 * AVDTP signaling: message framing, fragmentation, service capabilities.
 * Reference: AVDTP 1.3 spec, sections 8.4 (packet format) and 8.21 (capabilities).
 *
 * Kernel-friendly: no heap, no CRT beyond memcpy/memset.
 */
#ifndef AVDTP_H
#define AVDTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVDTP_PSM 0x0019
#define AVDTP_VERSION_1_3 0x0103

/* Message type, bits 1..0 of the first header byte. */
enum avdtp_msg_type {
    AVDTP_MSG_COMMAND = 0,
    AVDTP_MSG_GENERAL_REJECT = 1,
    AVDTP_MSG_ACCEPT = 2,
    AVDTP_MSG_REJECT = 3,
};

/* Packet type, bits 3..2 of the first header byte. */
enum avdtp_pkt_type {
    AVDTP_PKT_SINGLE = 0,
    AVDTP_PKT_START = 1,
    AVDTP_PKT_CONTINUE = 2,
    AVDTP_PKT_END = 3,
};

enum avdtp_signal {
    AVDTP_DISCOVER = 0x01,
    AVDTP_GET_CAPABILITIES = 0x02,
    AVDTP_SET_CONFIGURATION = 0x03,
    AVDTP_GET_CONFIGURATION = 0x04,
    AVDTP_RECONFIGURE = 0x05,
    AVDTP_OPEN = 0x06,
    AVDTP_START = 0x07,
    AVDTP_CLOSE = 0x08,
    AVDTP_SUSPEND = 0x09,
    AVDTP_ABORT = 0x0A,
    AVDTP_SECURITY_CONTROL = 0x0B,
    AVDTP_GET_ALL_CAPABILITIES = 0x0C,
    AVDTP_DELAYREPORT = 0x0D,
};

enum avdtp_category {
    AVDTP_CAT_MEDIA_TRANSPORT = 0x01,
    AVDTP_CAT_REPORTING = 0x02,
    AVDTP_CAT_RECOVERY = 0x03,
    AVDTP_CAT_CONTENT_PROTECTION = 0x04,
    AVDTP_CAT_HEADER_COMPRESSION = 0x05,
    AVDTP_CAT_MULTIPLEXING = 0x06,
    AVDTP_CAT_MEDIA_CODEC = 0x07,
    AVDTP_CAT_DELAY_REPORTING = 0x08,
};

enum avdtp_error {
    AVDTP_ERR_NONE = 0x00,
    AVDTP_ERR_BAD_HEADER_FORMAT = 0x01,
    AVDTP_ERR_BAD_LENGTH = 0x11,
    AVDTP_ERR_BAD_ACP_SEID = 0x12,
    AVDTP_ERR_SEP_IN_USE = 0x13,
    AVDTP_ERR_SEP_NOT_IN_USE = 0x14,
    AVDTP_ERR_BAD_SERV_CATEGORY = 0x17,
    AVDTP_ERR_BAD_PAYLOAD_FORMAT = 0x18,
    AVDTP_ERR_NOT_SUPPORTED_COMMAND = 0x19,
    AVDTP_ERR_INVALID_CAPABILITIES = 0x1A,
    AVDTP_ERR_BAD_MEDIA_TRANSPORT_FORMAT = 0x23,
    AVDTP_ERR_UNSUPPORTED_CONFIGURATION = 0x29,
    AVDTP_ERR_BAD_STATE = 0x31,
    /* A2DP-specific (A2DP spec, table 5.5) */
    A2DP_ERR_INVALID_CODEC_TYPE = 0xC1,
    A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE = 0xC2,
    A2DP_ERR_INVALID_SAMPLING_FREQUENCY = 0xC3,
    A2DP_ERR_INVALID_CHANNEL_MODE = 0xC5,
};

#define AVDTP_MEDIA_AUDIO 0x00
#define AVDTP_TSEP_SRC 0
#define AVDTP_TSEP_SNK 1

/* Default L2CAP signaling MTU; incoming messages larger than this are
 * fragmented by the peer, we reassemble up to AVDTP_SIG_MAX bytes. */
#define AVDTP_SIG_MTU_DEFAULT 672
#define AVDTP_SIG_MAX 1024
#define AVDTP_MAX_CODEC_INFO 32
#define AVDTP_MAX_SEPS 16

typedef struct avdtp_sep_info {
    uint8_t seid;   /* 1..62 */
    uint8_t in_use;
    uint8_t media_type;
    uint8_t tsep;
} avdtp_sep_info;

/* A fully reassembled signaling message. `payload` points after the
 * signal-id byte (into the caller's packet or the reassembly buffer). */
typedef struct avdtp_msg {
    uint8_t label;
    uint8_t msg_type;
    uint8_t signal;
    const uint8_t *payload;
    size_t len;
} avdtp_msg;

typedef struct avdtp_reasm {
    uint8_t buf[AVDTP_SIG_MAX];
    size_t len;
    uint8_t active;
    uint8_t nosp;
    uint8_t got;
    uint8_t label;
    uint8_t msg_type;
    uint8_t signal;
} avdtp_reasm;

/* Builds a single-packet message. Returns total length or 0 if it doesn't fit. */
size_t avdtp_build(uint8_t *buf, size_t cap, uint8_t label, uint8_t msg_type,
                   uint8_t signal, const uint8_t *payload, size_t len);

/* Feeds one L2CAP SDU. Returns 1 when `out` holds a complete message,
 * 0 when more fragments are needed, -1 on a malformed packet. */
int avdtp_reasm_push(avdtp_reasm *r, const uint8_t *pkt, size_t len, avdtp_msg *out);

/* Discover response: 2 bytes per SEP. */
size_t avdtp_discover_build(uint8_t *buf, size_t cap, const avdtp_sep_info *seps, size_t n);
size_t avdtp_discover_parse(const uint8_t *p, size_t len, avdtp_sep_info *seps, size_t max);

/* Parsed service capabilities (only what A2DP needs). */
typedef struct avdtp_caps {
    uint8_t media_transport;
    uint8_t delay_reporting;
    uint8_t content_protection;
    uint8_t has_codec;
    uint8_t media_type;
    uint8_t codec_type;
    uint8_t codec_info_len;
    uint8_t codec_info[AVDTP_MAX_CODEC_INFO];
} avdtp_caps;

/* Returns AVDTP_ERR_NONE or an error code; on error *bad_cat (if non-NULL)
 * receives the offending service category (needed for SET_CONFIGURATION reject). */
int avdtp_caps_parse(const uint8_t *p, size_t len, avdtp_caps *out, uint8_t *bad_cat);

/* Serializes capabilities. `with_delay_reporting` must be 0 for a plain
 * GET_CAPABILITIES response (delay reporting is a GET_ALL_CAPABILITIES item). */
size_t avdtp_caps_build(const avdtp_caps *c, int with_delay_reporting, uint8_t *buf, size_t cap);

const char *avdtp_signal_name(uint8_t signal);

#ifdef __cplusplus
}
#endif

#endif /* AVDTP_H */
