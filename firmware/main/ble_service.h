#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// recordPan GATT service; see docs/BLE.md for the protocol.
// Remote start/stop/mark commands are posted to `app_events` as btn_event_t,
// so they take the same path as the physical buttons.
esp_err_t ble_service_init(QueueHandle_t app_events);

// Pushes a status notification now (call after recording starts/stops).
void ble_service_status_changed(void);

// Streams one encoded Opus packet to a subscribed phone. `frame` is the
// 20 ms frame index since the recording started. Never blocks; drops on congestion.
void ble_service_audio_packet(const uint8_t *packet, size_t len, uint32_t frame);
