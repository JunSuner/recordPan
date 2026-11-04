#include "wav_writer.h"

#include <stddef.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    char riff[4];
    uint32_t riff_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} wav_header_t;

_Static_assert(sizeof(wav_header_t) == 44, "WAV header must be 44 bytes");

FILE *wav_open(const char *path, uint32_t sample_rate, uint16_t bits, uint16_t channels)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return NULL;
    }
    wav_header_t h = {
        .fmt_size = 16,
        .audio_format = 1,  // PCM
        .channels = channels,
        .sample_rate = sample_rate,
        .byte_rate = sample_rate * channels * bits / 8,
        .block_align = channels * bits / 8,
        .bits_per_sample = bits,
        .riff_size = sizeof(wav_header_t) - 8,
    };
    memcpy(h.riff, "RIFF", 4);
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt, "fmt ", 4);
    memcpy(h.data, "data", 4);
    if (fwrite(&h, sizeof(h), 1, f) != 1) {
        fclose(f);
        return NULL;
    }
    return f;
}

void wav_update_sizes(FILE *f, uint32_t data_bytes)
{
    uint32_t riff_size = data_bytes + sizeof(wav_header_t) - 8;
    fseek(f, offsetof(wav_header_t, riff_size), SEEK_SET);
    fwrite(&riff_size, sizeof(riff_size), 1, f);
    fseek(f, offsetof(wav_header_t, data_size), SEEK_SET);
    fwrite(&data_bytes, sizeof(data_bytes), 1, f);
    fseek(f, 0, SEEK_END);
}
