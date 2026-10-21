/*
 * A2DP media packets: RTP header (RFC 3550, 12 bytes, no CSRC/extension)
 * followed by a 1-byte codec media payload header. For both SBC and LDAC
 * the low nibble of that byte is the number of frames in the packet.
 */
#ifndef AVDTP_MEDIA_H
#define AVDTP_MEDIA_H

#include "avdtp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AVDTP_RTP_HDR_LEN 12
#define A2DP_MEDIA_PAYLOAD_HDR_LEN 1
#define A2DP_MEDIA_HDR_LEN (AVDTP_RTP_HDR_LEN + A2DP_MEDIA_PAYLOAD_HDR_LEN)
#define AVDTP_RTP_PT_DYNAMIC 96

typedef struct avdtp_media_stream {
    uint16_t seq;
    uint32_t timestamp; /* in samples per channel */
    uint32_t ssrc;
    uint8_t payload_type;
} avdtp_media_stream;

void avdtp_media_init(avdtp_media_stream *s, uint32_t ssrc);

/* Builds one media packet out of `nframes` codec frames and advances
 * seq/timestamp by `samples` (PCM samples per channel the frames cover).
 * Returns packet length, or 0 if it would exceed `mtu`. */
size_t avdtp_media_packet(avdtp_media_stream *s, uint8_t *out, size_t mtu,
                          const uint8_t *frames, size_t len, unsigned nframes, uint32_t samples);

#ifdef __cplusplus
}
#endif

#endif /* AVDTP_MEDIA_H */
