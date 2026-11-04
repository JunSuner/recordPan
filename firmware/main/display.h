#pragma once

#include "esp_err.h"

// Starts the 128x64 SSD1306 status screen if one answers on I2C (0x3C or 0x3D).
// Returns ESP_ERR_NOT_FOUND when no display is connected; recording works without it.
esp_err_t display_init(void);
