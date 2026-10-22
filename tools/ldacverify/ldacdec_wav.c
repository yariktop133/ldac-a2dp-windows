/*
 * ldacdec_wav: raw LDAC frames (ldacenc --raw, ldacsvc --dump) -> 32-bit PCM WAV,
 * using the open-source libldacdec (built by build-ldacdec.py). Verification only.
 *
 *   ldacdec_wav in.ldac out.wav
 */
#include "ldacdec.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void write_header(FILE *f, uint32_t rate, uint16_t ch, uint32_t data_bytes)
{
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    wr32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    wr32(h + 16, 16);
    wr16(h + 20, 1);
    wr16(h + 22, ch);
    wr32(h + 24, rate);
    wr32(h + 28, rate * ch * 4);
    wr16(h + 32, (uint16_t)(ch * 4));
    wr16(h + 34, 32);
    memcpy(h + 36, "data", 4);
    wr32(h + 40, data_bytes);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
}

int main(int argc, char **argv)
{
    static int32_t pcm[MAX_FRAME_SAMPLES * 2];
    static ldacdec_t dec;
    FILE *in, *out;
    uint8_t *buf;
    long size, pos = 0;
    uint32_t data_bytes = 0, frames = 0;
    int rate = 0, ch = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: ldacdec_wav in.ldac out.wav\n");
        return 2;
    }
    in = fopen(argv[1], "rb");
    if (!in)
        return perror(argv[1]), 1;
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    buf = calloc((size_t)size + 1024, 1); /* slack: the bit reader may read past a frame */
    if (!buf || fread(buf, 1, (size_t)size, in) != (size_t)size)
        return fprintf(stderr, "read error\n"), 1;
    fclose(in);

    out = fopen(argv[2], "wb");
    if (!out)
        return perror(argv[2]), 1;
    fseek(out, 44, SEEK_SET);

    ldacdecInit(&dec);
    while (pos < size) {
        int used = 0;
        if (buf[pos] != 0xAA) { /* resync on the LDAC frame sync byte */
            pos++;
            continue;
        }
        if (ldacDecode_type(&dec, buf + pos, pcm, &used, LDACBT_SMPL_FMT_S32) < 0 || used <= 0)
            break;
        rate = ldacdecGetSampleRate(&dec);
        ch = ldacdecGetChannelCount(&dec);
        fwrite(pcm, 4, (size_t)dec.frame.frameSamples * ch, out);
        data_bytes += (uint32_t)(dec.frame.frameSamples * ch * 4);
        frames++;
        pos += used;
    }
    write_header(out, (uint32_t)rate, (uint16_t)ch, data_bytes);
    fclose(out);
    printf("decoded %u frames, %d Hz, %d ch, %.3f s\n", frames, rate, ch,
           rate && ch ? (double)data_bytes / 4 / ch / rate : 0.0);
    return frames ? 0 : 1;
}
