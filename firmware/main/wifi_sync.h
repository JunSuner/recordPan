#pragma once

#include <stdbool.h>
#include "esp_err.h"

// On-demand Wi-Fi access point "recordPan-XXXX" with an HTTP server for bulk
// download and deletion of recordings (API in docs/WIFI.md). The WPA2 password
// is random per boot and handed to the phone over BLE. The AP turns itself off
// after WIFI_IDLE_OFF_S without HTTP requests.
esp_err_t wifi_sync_init(void);
esp_err_t wifi_sync_start(void);
void wifi_sync_stop(void);
bool wifi_sync_is_on(void);

// "SSID\nPASSWORD\nIP" for the BLE Wi-Fi characteristic.
const char *wifi_sync_credentials(void);

// Deletes an audio file and its .json metadata. Refuses the recording in progress
// and names that aren't plain REC_ files.
esp_err_t wifi_sync_delete_recording(const char *name);
