/*
 * ldacenc: WAV -> LDAC A2DP media packets, with a simulated radio link.
 *
 *   ldacenc in.wav [--quality hq|sq|mq|abr] [--mtu N] [--raw out.ldac]
 *                  [--packets out.bin] [--link T:KBPS,...] [--tick-ms N] [-v]
 *
 * --raw      concatenated LDAC frames (for libldacdec / ffmpeg-style decoders)
 * --packets  A2DP media packets as on the L2CAP channel: [u16le len][RTP+hdr+frames]...
 * --link     piecewise link capacity, e.g. "0:1200,5:300,10:1200" (seconds:kbit/s).
 *            Packets go into a TX queue that drains at that rate; with --quality abr
 *            the queue depth drives libldac's adaptive bit rate like Android does.
 */
#include "avdtp_media.h"
#include "ldacBT.h"
#include "ldacBT_abr.h"
#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINK_POINTS 32
#define TXQ_MAX 64 /* packets; beyond this the sender drops, like a full ACL buffer */

typedef struct link_point {
    double t;
    double kbps;
} link_point;

static int parse_link(const char *s, link_point *pts, int max)
{
    int n = 0;
    while (*s && n < max) {
        char *end;
        pts[n].t = strtod(s, &end);
        if (*end != ':')
            return -1;
        pts[n].kbps = strtod(end + 1, &end);
        n++;
        if (*end == ',')
            end++;
        else if (*end)
            return -1;
        s = end;
    }
    return n;
}

static double link_kbps(const link_point *pts, int n, double t)
{
    double k = n ? pts[0].kbps : 1e9;
    int i;
    for (i = 0; i < n; i++)
        if (t >= pts[i].t)
            k = pts[i].kbps;
    return k;
}

/* EQMID values: HQ/SQ/MQ are public; ABR also steps through the extended
 * Q0 (492 kbps) and Q1 (396 kbps) from libldac's ldacBT_ex.h. */
#define EQMID_COUNT 12
static const char *eqmid_name(int e)
{
    static const char *const names[EQMID_COUNT] = { "HQ", "SQ", "MQ", "Q0", "Q1", "Q2",
                                                     "Q3", "Q4", "Q5", "Q6", "Q7", "Q8" };
    return e >= 0 && e < EQMID_COUNT ? names[e] : "?";
}

/* Nominal bit rate of an EQMID (libldac ldacBT_internal.c table, 48/96 kHz;
 * 44.1/88.2 kHz scale by 44.1/48). */
static int eqmid_kbps(int e, uint32_t rate)
{
    static const int kbps[EQMID_COUNT] = { 990, 660, 330, 492, 396, 282, 246, 216, 198, 180, 162, 150 };
    if (e < 0 || e >= EQMID_COUNT)
        return 0;
    return rate % 44100 == 0 ? kbps[e] * 441 / 480 : kbps[e];
}

static void usage(void)
{
    fprintf(stderr,
            "usage: ldacenc in.wav [--quality hq|sq|mq|abr] [--mtu N] [--raw out.ldac]\n"
            "                      [--packets out.bin] [--link T:KBPS,...] [--tick-ms N] [-v]\n");
}

int main(int argc, char **argv)
{
    const char *in = NULL, *raw_path = NULL, *pkt_path = NULL;
    int eqmid = LDACBT_EQMID_SQ, abr = 0, mtu = 679 + 0, tick_ms = 20, verbose = 0;
    link_point link[MAX_LINK_POINTS];
    int nlink = 0;
    wav_file w;
    LDACBT_SMPL_FMT_T fmt;
    HANDLE_LDAC_BT h;
    HANDLE_LDAC_ABR habr = NULL;
    FILE *raw = NULL, *pkts = NULL;
    avdtp_media_stream ms;
    int i, cm;

    /* PCM in, encoded stream and packet out */
    uint8_t pcm[LDACBT_ENC_LSU * 2 * 4];
    uint8_t stream[LDACBT_MAX_NBYTES];
    uint8_t packet[2048];

    /* stats */
    unsigned long long n_packets = 0, n_frames = 0, payload_bytes = 0, samples_in = 0, dropped = 0;
    size_t min_pkt = (size_t)-1, max_pkt = 0;
    double eqmid_time[EQMID_COUNT] = { 0 };
    int last_eqmid;

    /* TX queue simulation */
    size_t txq[TXQ_MAX];
    size_t txq_head = 0, txq_len = 0;
    double drain_credit = 0, next_tick = 0;
    unsigned max_depth = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quality") && i + 1 < argc) {
            const char *q = argv[++i];
            if (!strcmp(q, "hq")) eqmid = LDACBT_EQMID_HQ;
            else if (!strcmp(q, "sq")) eqmid = LDACBT_EQMID_SQ;
            else if (!strcmp(q, "mq")) eqmid = LDACBT_EQMID_MQ;
            else if (!strcmp(q, "abr")) abr = 1, eqmid = LDACBT_EQMID_HQ;
            else return usage(), 2;
        } else if (!strcmp(argv[i], "--mtu") && i + 1 < argc) {
            mtu = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--raw") && i + 1 < argc) {
            raw_path = argv[++i];
        } else if (!strcmp(argv[i], "--packets") && i + 1 < argc) {
            pkt_path = argv[++i];
        } else if (!strcmp(argv[i], "--link") && i + 1 < argc) {
            nlink = parse_link(argv[++i], link, MAX_LINK_POINTS);
            if (nlink <= 0)
                return fprintf(stderr, "bad --link\n"), 2;
        } else if (!strcmp(argv[i], "--tick-ms") && i + 1 < argc) {
            tick_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if (argv[i][0] != '-' && !in) {
            in = argv[i];
        } else {
            return usage(), 2;
        }
    }
    if (!in)
        return usage(), 2;
    if (mtu > (int)sizeof(packet))
        return fprintf(stderr, "mtu too large\n"), 2;

    if (wav_open(&w, in) != 0)
        return 1;
    switch (w.format) {
    case WAV_S16: fmt = LDACBT_SMPL_FMT_S16; break;
    case WAV_S24: fmt = LDACBT_SMPL_FMT_S24; break;
    case WAV_S32: fmt = LDACBT_SMPL_FMT_S32; break;
    default: fmt = LDACBT_SMPL_FMT_F32; break;
    }
    if (w.channels != 1 && w.channels != 2)
        return fprintf(stderr, "only mono/stereo input\n"), 1;
    cm = w.channels == 1 ? LDACBT_CHANNEL_MODE_MONO : LDACBT_CHANNEL_MODE_STEREO;

    h = ldacBT_get_handle();
    if (!h || ldacBT_init_handle_encode(h, mtu, eqmid, cm, fmt, (int)w.sample_rate) != 0) {
        fprintf(stderr, "ldacBT_init_handle_encode failed: error %d (rate %u, mtu %d)\n",
                h ? ldacBT_get_error_code(h) : -1, w.sample_rate, mtu);
        return 1;
    }
    if (abr) {
        habr = ldac_ABR_get_handle();
        if (!habr || ldac_ABR_Init(habr, (unsigned)tick_ms) != 0)
            return fprintf(stderr, "ldac_ABR_Init failed\n"), 1;
    }
    if (raw_path && !(raw = fopen(raw_path, "wb")))
        return fprintf(stderr, "cannot create %s\n", raw_path), 1;
    if (pkt_path && !(pkts = fopen(pkt_path, "wb")))
        return fprintf(stderr, "cannot create %s\n", pkt_path), 1;
    avdtp_media_init(&ms, 0x4C444143 /* "LDAC" */);

    printf("input: %u Hz, %u ch, %u-bit%s; LDAC %s, MTU %d, libldac %06X\n", w.sample_rate, w.channels,
           w.bytes_per_sample * 8, w.format == WAV_F32 ? " float" : "", abr ? "ABR" : eqmid_name(eqmid), mtu,
           ldacBT_get_version());

    last_eqmid = ldacBT_get_eqmid(h);
    for (;;) {
        size_t got = wav_read(&w, pcm, LDACBT_ENC_LSU);
        int pcm_used = 0, sz = 0, nfr = 0, flushing = got == 0;
        double t = (double)samples_in / w.sample_rate;
        double dt = (double)LDACBT_ENC_LSU / w.sample_rate;

        if (got > 0 && got < LDACBT_ENC_LSU)
            memset(pcm + got * w.channels * w.bytes_per_sample, 0,
                   (LDACBT_ENC_LSU - got) * w.channels * w.bytes_per_sample);

        if (ldacBT_encode(h, flushing ? NULL : pcm, &pcm_used, stream, &sz, &nfr) != 0) {
            fprintf(stderr, "ldacBT_encode failed: error %d\n", ldacBT_get_error_code(h));
            return 1;
        }
        if (!flushing)
            samples_in += LDACBT_ENC_LSU;

        if (sz > 0) {
            size_t n = avdtp_media_packet(&ms, packet, (size_t)mtu, stream, (size_t)sz, (unsigned)nfr,
                                          /* RTP timestamp advances 128 per LDAC frame at every
                                           * rate, as captured from Alternative A2DP -> JBL
                                           * (docs/jbl-live-780nc.md), even though a 96 kHz
                                           * frame holds 256 samples. */
                                          (uint32_t)(nfr * LDACBT_ENC_LSU));
            if (!n) {
                fprintf(stderr, "packet of %d bytes / %d frames does not fit MTU %d\n", sz, nfr, mtu);
                return 1;
            }
            if (raw)
                fwrite(stream, 1, (size_t)sz, raw);
            if (pkts) {
                uint8_t len[2] = { (uint8_t)n, (uint8_t)(n >> 8) };
                fwrite(len, 1, 2, pkts);
                fwrite(packet, 1, n, pkts);
            }
            n_packets++;
            n_frames += (unsigned)nfr;
            payload_bytes += (unsigned)sz;
            if (n < min_pkt) min_pkt = n;
            if (n > max_pkt) max_pkt = n;

            if (txq_len < TXQ_MAX)
                txq[(txq_head + txq_len++) % TXQ_MAX] = n;
            else
                dropped++;
        }

        /* drain the simulated link */
        drain_credit += link_kbps(link, nlink, t) * 1000.0 / 8.0 * dt;
        while (txq_len && drain_credit >= (double)txq[txq_head]) {
            drain_credit -= (double)txq[txq_head];
            txq_head = (txq_head + 1) % TXQ_MAX;
            txq_len--;
        }
        if (!txq_len && drain_credit > 4096)
            drain_credit = 4096; /* idle link doesn't bank unlimited capacity */
        if (txq_len > max_depth)
            max_depth = (unsigned)txq_len;

        if (!flushing) {
            int e = ldacBT_get_eqmid(h);
            if (e >= 0 && e < EQMID_COUNT)
                eqmid_time[e] += dt;
        }
        if (t >= next_tick) {
            next_tick += tick_ms / 1000.0;
            if (habr)
                ldac_ABR_Proc(h, habr, (unsigned)txq_len, 1);
            if (ldacBT_get_eqmid(h) != last_eqmid) {
                if (verbose || habr)
                    printf("  t=%7.3fs  %s -> %s (%d kbps)  txq %u, link %.0f kbps\n", t, eqmid_name(last_eqmid),
                           eqmid_name(ldacBT_get_eqmid(h)), eqmid_kbps(ldacBT_get_eqmid(h), w.sample_rate), (unsigned)txq_len,
                           link_kbps(link, nlink, t));
                last_eqmid = ldacBT_get_eqmid(h);
            }
        }

        if (flushing && sz <= 0)
            break;
    }

    {
        double secs = (double)samples_in / w.sample_rate;
        printf("duration      %.3f s\n", secs);
        printf("packets       %llu (%.1f/s), frames %llu (%.2f per packet)\n", n_packets, n_packets / secs,
               n_frames, n_packets ? (double)n_frames / n_packets : 0.0);
        printf("packet size   min %zu, max %zu, avg %.1f bytes (MTU %d)\n", n_packets ? min_pkt : 0, max_pkt,
               n_packets ? (double)(payload_bytes + n_packets * A2DP_MEDIA_HDR_LEN) / n_packets : 0.0, mtu);
        printf("bitrate       %.1f kbit/s codec payload, %.1f kbit/s on air (with RTP+media hdr)\n",
               payload_bytes * 8.0 / secs / 1000.0,
               (payload_bytes + n_packets * A2DP_MEDIA_HDR_LEN) * 8.0 / secs / 1000.0);
        printf("quality time ");
        for (i = 0; i < EQMID_COUNT; i++)
            if (eqmid_time[i] > 0)
                printf(" %s %.1f s", eqmid_name(i), eqmid_time[i]);
        printf("\n");
        if (nlink)
            printf("tx queue      max depth %u packets, dropped %llu\n", max_depth, dropped);
    }

    if (raw) fclose(raw);
    if (pkts) fclose(pkts);
    if (habr) ldac_ABR_free_handle(habr);
    ldacBT_free_handle(h);
    wav_close(&w);
    return 0;
}
