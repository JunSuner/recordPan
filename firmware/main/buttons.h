#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    BTN_EVT_REC,   // record button pressed
    BTN_EVT_MARK,  // mark button pressed
} btn_event_t;

// Starts a polling task that posts btn_event_t items to `queue` on each press.
void buttons_init(QueueHandle_t queue);
