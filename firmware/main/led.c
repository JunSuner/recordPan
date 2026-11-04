#include "led.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"

#define FLASH_MS 150
#define TICK_MS  50

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static volatile led_state_t s_state = LED_IDLE;
static volatile TickType_t s_flash_until;

static void led_task(void *arg)
{
    bool blink_on = false;
    while (true) {
        uint8_t r = 0, g = 0, b = 0;
        if (xTaskGetTickCount() < s_flash_until) {
            b = 40;
        } else {
            switch (s_state) {
            case LED_IDLE:
                g = 6;
                break;
            case LED_RECORDING:
                r = 30;
                break;
            case LED_ERROR:
                blink_on = !blink_on;
                r = blink_on ? 40 : 0;
                break;
            }
        }
        led_strip_set_pixel(s_strip, 0, r, g, b);
        led_strip_refresh(s_strip);
        vTaskDelay(pdMS_TO_TICKS(s_state == LED_ERROR ? 250 : TICK_MS));
    }
}

void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_RP_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    ESP_LOGI(TAG, "LED on GPIO%d", CONFIG_RP_LED_GPIO);
}

void led_set_state(led_state_t state)
{
    s_state = state;
}

void led_flash(void)
{
    s_flash_until = xTaskGetTickCount() + pdMS_TO_TICKS(FLASH_MS);
}
