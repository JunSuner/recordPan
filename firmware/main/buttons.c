#include "buttons.h"

#include "driver/gpio.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define POLL_MS        10
#define DEBOUNCE_COUNT 3  // stable samples before a level change is accepted

typedef struct {
    gpio_num_t gpio;
    btn_event_t event;
    bool pressed;
    int stable;
} button_t;

static button_t s_buttons[] = {
    { .gpio = CONFIG_RP_BTN_REC_GPIO,  .event = BTN_EVT_REC },
    { .gpio = CONFIG_RP_BTN_MARK_GPIO, .event = BTN_EVT_MARK },
};

static QueueHandle_t s_queue;

static void buttons_task(void *arg)
{
    while (true) {
        for (int i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++) {
            button_t *b = &s_buttons[i];
            bool level_pressed = gpio_get_level(b->gpio) == 0;  // active low
            if (level_pressed == b->pressed) {
                b->stable = 0;
                continue;
            }
            if (++b->stable >= DEBOUNCE_COUNT) {
                b->pressed = level_pressed;
                b->stable = 0;
                if (b->pressed) {
                    xQueueSend(s_queue, &b->event, 0);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_init(QueueHandle_t queue)
{
    s_queue = queue;
    for (int i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_buttons[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
    }
    xTaskCreate(buttons_task, "buttons", 2048, NULL, 5, NULL);
}
