#include "buzzer.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define SHORT_MS 60
#define GAP_MS   80
#define LONG_MS  600

static QueueHandle_t s_queue;

static void beep(int ms)
{
    gpio_set_level(CONFIG_RP_BUZZER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(CONFIG_RP_BUZZER_GPIO, 0);
}

static void buzzer_task(void *arg)
{
    buzzer_pattern_t p;
    while (true) {
        xQueueReceive(s_queue, &p, portMAX_DELAY);
        switch (p) {
        case BUZZER_START:
            beep(SHORT_MS);
            break;
        case BUZZER_STOP:
            beep(SHORT_MS);
            vTaskDelay(pdMS_TO_TICKS(GAP_MS));
            beep(SHORT_MS);
            break;
        case BUZZER_ERROR:
            beep(LONG_MS);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(GAP_MS));
    }
}

void buzzer_init(void)
{
    if (CONFIG_RP_BUZZER_GPIO < 0) {
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_RP_BUZZER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(CONFIG_RP_BUZZER_GPIO, 0);
    s_queue = xQueueCreate(4, sizeof(buzzer_pattern_t));
    xTaskCreate(buzzer_task, "buzzer", 2048, NULL, 4, NULL);
}

void buzzer_play(buzzer_pattern_t pattern)
{
    if (s_queue) {
        xQueueSend(s_queue, &pattern, 0);
    }
}
