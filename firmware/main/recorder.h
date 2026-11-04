#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RECORDER_SAMPLE_RATE 16000

esp_err_t recorder_init(void);

// Opens the next REC_NNNN.opus (or .wav, see menuconfig) and starts capturing.
esp_err_t recorder_start(void);

// Stops capturing, drains buffered audio to the file and closes it.
// Writes REC_NNNN.json with the duration and the mark offsets.
esp_err_t recorder_stop(void);

bool recorder_is_recording(void);

// Records a mark at the current position. Returns the offset in ms, or -1.
int32_t recorder_mark(void);

// Elapsed recording time in ms.
uint32_t recorder_elapsed_ms(void);

int recorder_mark_count(void);

// Input level of the last 20 ms in dBFS (-96 when silent or idle).
int recorder_peak_db(void);

// Base name of the current or last recording, e.g. "REC_0007" ("" before the first).
const char *recorder_file_name(void);

// Channels stored in the audio file (Opus is always mono).
int recorder_file_channels(void);
