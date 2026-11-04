#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Minimal Ogg Opus (RFC 7845) muxer: one logical stream, one Opus packet per
// encoded frame, pages flushed about once a second.
typedef struct {
    FILE *f;
    uint32_t serial;
    uint32_t page_seq;
    uint64_t granule;       // 48 kHz samples up to the last packet added
    uint8_t segs[255];      // lacing values of the pending page
    int seg_count;
    int packet_count;       // packets in the pending page
    uint8_t body[8192];     // payload of the pending page
    size_t body_len;
} ogg_opus_t;

// Creates `path` and writes the OpusHead and OpusTags pages.
bool ogg_opus_open(ogg_opus_t *o, const char *path, uint32_t input_rate, uint8_t channels, uint16_t pre_skip);

// Appends one Opus packet covering `samples_48k` samples (960 for 20 ms).
// Completed pages are written to the file; returns false on a write error.
bool ogg_opus_write(ogg_opus_t *o, const uint8_t *packet, size_t len, uint32_t samples_48k);

// Writes the pending page (if any) so the file is complete up to now.
bool ogg_opus_flush(ogg_opus_t *o);

// Writes the final page with the end-of-stream flag and closes the file.
void ogg_opus_close(ogg_opus_t *o);
