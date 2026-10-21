#ifndef WAV_H
#define WAV_H

#include <stdint.h>
#include <stdio.h>

typedef enum { WAV_S16, WAV_S24, WAV_S32, WAV_F32 } wav_format;

typedef struct wav_file {
    FILE *f;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bytes_per_sample;
    wav_format format;
    uint64_t data_left; /* bytes */
} wav_file;

/* Returns 0 on success, prints a reason to stderr otherwise. */
int wav_open(wav_file *w, const char *path);
/* Reads up to `frames` interleaved frames; returns frames read. */
size_t wav_read(wav_file *w, void *buf, size_t frames);
void wav_close(wav_file *w);

#endif
