#pragma once

#include <stdint.h>
#include <stdio.h>

// Opens `path` and writes a 44-byte PCM WAV header with zero data length.
FILE *wav_open(const char *path, uint32_t sample_rate, uint16_t bits, uint16_t channels);

// Rewrites the RIFF/data sizes so the file is playable up to `data_bytes`,
// then restores the write position to the end of the file.
void wav_update_sizes(FILE *f, uint32_t data_bytes);
