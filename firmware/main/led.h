#pragma once

typedef enum {
    LED_IDLE,       // dim green: ready
    LED_RECORDING,  // red: recording
    LED_ERROR,      // blinking red: SD card or mic failure
} led_state_t;

void led_init(void);
void led_set_state(led_state_t state);
// Short blue flash on top of the current state (used for marks).
void led_flash(void);
