#include "wav.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int wav_open(wav_file *w, const char *path)
{
    uint8_t hdr[12], ck[8], fmt[40];
    int have_fmt = 0;

    memset(w, 0, sizeof(*w));
    w->f = fopen(path, "rb");
    if (!w->f) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    if (fread(hdr, 1, 12, w->f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
        goto fail;
    }
    while (fread(ck, 1, 8, w->f) == 8) {
        uint32_t size = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            uint16_t tag, bits;
            uint32_t n = size < sizeof(fmt) ? size : (uint32_t)sizeof(fmt);
            if (size < 16 || fread(fmt, 1, n, w->f) != n)
                goto fail;
            if (size > n)
                fseek(w->f, (long)(size - n), SEEK_CUR);
            tag = rd16(fmt);
            w->channels = rd16(fmt + 2);
            w->sample_rate = rd32(fmt + 4);
            bits = rd16(fmt + 14);
            if (tag == 0xFFFE && size >= 26)
                tag = rd16(fmt + 24); /* WAVE_FORMAT_EXTENSIBLE sub-format GUID */
            w->bytes_per_sample = bits / 8;
            if (tag == 1 && bits == 16)
                w->format = WAV_S16;
            else if (tag == 1 && bits == 24)
                w->format = WAV_S24;
            else if (tag == 1 && bits == 32)
                w->format = WAV_S32;
            else if (tag == 3 && bits == 32)
                w->format = WAV_F32;
            else {
                fprintf(stderr, "%s: unsupported format tag %u, %u bits\n", path, tag, bits);
                goto fail;
            }
            have_fmt = 1;
        } else if (!memcmp(ck, "data", 4)) {
            if (!have_fmt) {
                fprintf(stderr, "%s: data before fmt\n", path);
                goto fail;
            }
            w->data_left = size;
            return 0;
        } else {
            fseek(w->f, (long)(size + (size & 1)), SEEK_CUR);
        }
    }
    fprintf(stderr, "%s: no data chunk\n", path);
fail:
    fclose(w->f);
    w->f = NULL;
    return -1;
}

size_t wav_read(wav_file *w, void *buf, size_t frames)
{
    size_t frame_bytes = (size_t)w->channels * w->bytes_per_sample;
    size_t want = frames * frame_bytes;
    size_t got;
    if (want > w->data_left)
        want = (size_t)(w->data_left / frame_bytes * frame_bytes);
    got = fread(buf, 1, want, w->f);
    w->data_left -= got;
    return got / frame_bytes;
}

void wav_close(wav_file *w)
{
    if (w->f)
        fclose(w->f);
    w->f = NULL;
}
