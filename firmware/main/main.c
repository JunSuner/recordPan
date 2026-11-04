#include "buttons.h"
#include "buzzer.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "led.h"
#include "recorder.h"
#include "storage.h"

static const char *TAG = "main";

static void toggle_recording(void)
{
    if (recorder_is_recording()) {
        recorder_stop();
        led_set_state(LED_IDLE);
        buzzer_play(BUZZER_STOP);  // after capture has stopped, so it isn't recorded
    } else if (recorder_start() == ESP_OK) {
        // The first 200 ms after start are discarded as mic warm-up, so this beep isn't recorded.
        buzzer_play(BUZZER_START);
        led_set_state(LED_RECORDING);
    } else {
        led_set_state(LED_ERROR);
        buzzer_play(BUZZER_ERROR);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "recordPan M2: press REC to start/stop, MARK to add a mark");

    led_init();
    buzzer_init();
    display_init();  // optional: recording works without a display
    QueueHandle_t events = xQueueCreate(8, sizeof(btn_event_t));
    buttons_init(events);
    ESP_ERROR_CHECK(recorder_init());

    // A missing card isn't fatal: pressing REC retries the mount.
    led_set_state(storage_mount() == ESP_OK ? LED_IDLE : LED_ERROR);

    while (true) {
        btn_event_t evt;
        if (xQueueReceive(events, &evt, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (evt) {
            case BTN_EVT_REC:
                toggle_recording();
                break;
            case BTN_EVT_MARK:
                if (recorder_mark() >= 0) {
                    led_flash();
                }
                break;
            }
        } else if (recorder_is_recording()) {
            uint32_t s = recorder_elapsed_ms() / 1000;
            ESP_LOGI(TAG, "REC %02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
        }
    }
}
