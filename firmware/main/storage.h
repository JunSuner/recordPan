#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_RP_STORAGE_FLASH
#define STORAGE_MOUNT_POINT "/flash"
#else
#define STORAGE_MOUNT_POINT "/sdcard"
#endif

esp_err_t storage_mount(void);
bool storage_is_mounted(void);

// Free space in bytes, 0 when not mounted.
uint64_t storage_free_bytes(void);

// Writes the next free "<mount>/REC_NNNN" base path (no extension) into `out`.
esp_err_t storage_next_rec_base(char *out, size_t len);
