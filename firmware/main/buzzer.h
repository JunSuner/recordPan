#pragma once

typedef enum {
    BUZZER_START,  // one short beep
    BUZZER_STOP,   // two short beeps
    BUZZER_ERROR,  // one long beep
} buzzer_pattern_t;

// Active buzzer, high level = on. Does nothing if the GPIO is set to -1.
void buzzer_init(void);

// Queues a pattern and returns immediately.
void buzzer_play(buzzer_pattern_t pattern);
