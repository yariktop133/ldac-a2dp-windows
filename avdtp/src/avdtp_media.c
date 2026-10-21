#include "avdtp_media.h"

#include <string.h>

void avdtp_media_init(avdtp_media_stream *s, uint32_t ssrc)
{
    s->seq = 0;
    s->timestamp = 0;
    s->ssrc = ssrc ? ssrc : 1;
    s->payload_type = AVDTP_RTP_PT_DYNAMIC;
}

static void wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

size_t avdtp_media_packet(avdtp_media_stream *s, uint8_t *out, size_t mtu,
                          const uint8_t *frames, size_t len, unsigned nframes, uint32_t samples)
{
    if (nframes == 0 || nframes > 15 || len + A2DP_MEDIA_HDR_LEN > mtu)
        return 0;

    out[0] = 0x80; /* V=2, P=0, X=0, CC=0 */
    out[1] = s->payload_type & 0x7F;
    out[2] = (uint8_t)(s->seq >> 8);
    out[3] = (uint8_t)s->seq;
    wr32be(out + 4, s->timestamp);
    wr32be(out + 8, s->ssrc);
    out[12] = (uint8_t)(nframes & 0x0F);
    memcpy(out + A2DP_MEDIA_HDR_LEN, frames, len);

    s->seq++;
    s->timestamp += samples;
    return len + A2DP_MEDIA_HDR_LEN;
}
