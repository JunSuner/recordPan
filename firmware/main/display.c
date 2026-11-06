#include "display.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "recorder.h"
#include "sdkconfig.h"
#include "storage.h"
#include "wifi_sync.h"

#define WIDTH       128
#define HEIGHT      64
#define REFRESH_MS  100
#define MARK_SHOW_MS 800

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
// SSD1306 page layout: byte (page * WIDTH + x) holds pixels y = page*8 .. page*8+7, LSB on top.
static uint8_t s_fb[WIDTH * HEIGHT / 8];

// 5x7 ASCII font, characters 0x20..0x7E, one byte per column, LSB on top.
static const uint8_t s_font[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x08, 0x2A, 0x1C, 0x2A, 0x08}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x01, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x32}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x08, 0x14, 0x54, 0x54, 0x3C},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00},
    {0x00, 0x7F, 0x10, 0x28, 0x44}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7C, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02},
};

static void set_px(int x, int y, bool on)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        return;
    }
    uint8_t *b = &s_fb[(y / 8) * WIDTH + x];
    *b = on ? (*b | (1 << (y % 8))) : (*b & ~(1 << (y % 8)));
}

static void fill_rect(int x, int y, int w, int h, bool on)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            set_px(i, j, on);
        }
    }
}

static void draw_rect(int x, int y, int w, int h)
{
    fill_rect(x, y, w, 1, true);
    fill_rect(x, y + h - 1, w, 1, true);
    fill_rect(x, y, 1, h, true);
    fill_rect(x + w - 1, y, 1, h, true);
}

// Draws text with its top-left corner at (x, y); each character cell is 6*scale x 8*scale.
static void draw_text(int x, int y, const char *s, int scale, bool on)
{
    for (; *s; s++, x += 6 * scale) {
        char c = (*s < 0x20 || *s > 0x7E) ? '?' : *s;
        const uint8_t *glyph = s_font[c - 0x20];
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (glyph[col] & (1 << row)) {
                    fill_rect(x + col * scale, y + row * scale, scale, scale, on);
                }
            }
        }
    }
}

static int text_width(const char *s, int scale)
{
    return (int)strlen(s) * 6 * scale - scale;
}

static void format_remaining(char *out, size_t len, uint64_t free_bytes)
{
#if CONFIG_RP_REC_FORMAT_OPUS
    const uint32_t bytes_per_s = CONFIG_RP_OPUS_BITRATE / 8 + 60;  // + Ogg overhead
#else
    const uint32_t bytes_per_s = RECORDER_SAMPLE_RATE * 2 * recorder_file_channels();
#endif
    uint32_t min = (uint32_t)(free_bytes / bytes_per_s / 60);
    if (min >= 60) {
        snprintf(out, len, "%luh%02lum", (unsigned long)(min / 60), (unsigned long)(min % 60));
    } else {
        snprintf(out, len, "%lum", (unsigned long)min);
    }
}

typedef enum {
    EYES_OPEN,
    EYES_CLOSED,
    EYES_HAPPY,  // ^ ^
} eyes_t;

typedef struct {
    int lift;        // pixels the body is raised (bob / jump)
    bool left_up;    // arm raised
    bool right_up;
    eyes_t eyes;
    int mouth_h;     // mouth height in pixels, 0 = no mouth
    bool zzz;        // sleeping bubbles
    int zzz_phase;
} clawd_pose_t;

// Clawd, the Claude Code mascot, from the clawd-emotes block anatomy: an 11x7 torso,
// 2x2 hands, four 1x2 legs and a shadow, drawn at CLAWD_S pixels per unit.
#define CLAWD_S 3
#define CLAWD_W (15 * CLAWD_S)
#define CLAWD_H (10 * CLAWD_S)

static void clawd_rect(int x0, int y0, int x, int y, int w, int h, bool on)
{
    fill_rect(x0 + x * CLAWD_S, y0 + y * CLAWD_S, w * CLAWD_S, h * CLAWD_S, on);
}

static void draw_clawd(int x0, int y0, const clawd_pose_t *p)
{
    // Shadow stays on the ground (dotted = half tone); everything else lifts.
    for (int x = 3 * CLAWD_S; x < 12 * CLAWD_S; x += 2) {
        set_px(x0 + x, y0 + CLAWD_H - 2, true);
    }
    int y = y0 - p->lift;
    clawd_rect(x0, y, 2, 0, 11, 7, true);  // torso
    clawd_rect(x0, y, 3, 7, 1, 2, true);   // legs
    clawd_rect(x0, y, 5, 7, 1, 2, true);
    clawd_rect(x0, y, 9, 7, 1, 2, true);
    clawd_rect(x0, y, 11, 7, 1, 2, true);
    clawd_rect(x0, y, 0, p->left_up ? 1 : 3, 2, 2, true);  // hands
    clawd_rect(x0, y, 13, p->right_up ? 1 : 3, 2, 2, true);

    // Eyes and mouth are cut out of the torso.
    for (int i = 0; i < 2; i++) {
        int ex = x0 + (i ? 10 : 4) * CLAWD_S;
        int ey = y + 2 * CLAWD_S;
        switch (p->eyes) {
        case EYES_OPEN:
            fill_rect(ex, ey, CLAWD_S, 2 * CLAWD_S, false);
            break;
        case EYES_CLOSED:
            fill_rect(ex - 1, ey + 2 * CLAWD_S - 2, CLAWD_S + 2, 1, false);
            break;
        case EYES_HAPPY:
            set_px(ex + 1, ey + 2, false);
            set_px(ex, ey + 3, false);
            set_px(ex + 2, ey + 3, false);
            set_px(ex - 1, ey + 4, false);
            set_px(ex + 3, ey + 4, false);
            break;
        }
    }
    if (p->mouth_h > 0) {
        fill_rect(x0 + 6 * CLAWD_S + 1, y + 5 * CLAWD_S - 1, 2 * CLAWD_S + 1, p->mouth_h, false);
    }

    if (p->zzz) {
        // Small z's drifting up from the right of the head.
        static const char *z = "z";
        for (int i = 0; i < 2; i++) {
            int phase = (p->zzz_phase + i * 8) % 16;
            draw_text(x0 + CLAWD_W - 2 + phase / 3, y0 + 2 - phase / 2, z, 1, phase < 14);
        }
    }
}

static void render(uint64_t free_bytes, bool show_mark, bool blink, const clawd_pose_t *pose)
{
    char buf[24];
    memset(s_fb, 0, sizeof(s_fb));
    bool rec = recorder_is_recording();

    // Top line: state on the left, remaining recording time on the right.
    if (!storage_is_mounted()) {
        draw_text(0, 0, "NO STORAGE", 1, true);
    } else if (rec) {
        if (blink) {
            fill_rect(1, 1, 5, 5, true);
        }
        draw_text(9, 0, "REC", 1, true);
    } else {
        draw_text(0, 0, "READY", 1, true);
    }
    if (wifi_sync_is_on()) {
        draw_text(48, 0, "WiFi", 1, true);
    }
    if (storage_is_mounted()) {
        format_remaining(buf, sizeof(buf), free_bytes);
        draw_text(WIDTH - text_width(buf, 1), 0, buf, 1, true);
    }

    // Clawd on the left, elapsed time on the right.
    draw_clawd(4, 11, pose);
    uint32_t s = rec ? recorder_elapsed_ms() / 1000 : 0;
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(s / 60 % 100), (unsigned long)(s % 60));
    draw_text(WIDTH - text_width(buf, 2) - 2, 20, buf, 2, true);

    // Input level, -60..0 dBFS.
    draw_rect(0, 42, WIDTH, 7);
    int db = recorder_peak_db();
    int fill = db <= -60 ? 0 : (db >= 0 ? WIDTH - 4 : (db + 60) * (WIDTH - 4) / 60);
    fill_rect(2, 44, fill, 3, true);

    // Bottom line: file name and mark count; flashes inverted right after a mark.
    if (show_mark) {
        fill_rect(0, 54, WIDTH, 10, true);
        snprintf(buf, sizeof(buf), "MARK %d", recorder_mark_count());
        draw_text((WIDTH - text_width(buf, 1)) / 2, 55, buf, 1, false);
    } else {
        draw_text(0, 56, recorder_file_name(), 1, true);
        if (rec || recorder_mark_count() > 0) {
            snprintf(buf, sizeof(buf), "M%d", recorder_mark_count());
            draw_text(WIDTH - text_width(buf, 1), 56, buf, 1, true);
        }
    }
}

// Picks Clawd's pose for this frame:
// idle    - gentle bob, blinks every few seconds, dozes off after SLEEP_AFTER_TICKS;
// talking - bounces and waves its hands while the input is loud, mouth opens with the level;
// mark    - jumps with happy ^ ^ eyes.
#define SLEEP_AFTER_TICKS (60 * 1000 / REFRESH_MS)

static void pick_pose(clawd_pose_t *p, uint32_t tick, uint32_t idle_ticks, bool rec, bool marked)
{
    memset(p, 0, sizeof(*p));
    p->eyes = (tick % 35 == 0 || tick % 35 == 1) ? EYES_CLOSED : EYES_OPEN;
    int db = recorder_peak_db();
    if (marked) {
        p->lift = 3;
        p->left_up = p->right_up = true;
        p->eyes = EYES_HAPPY;
        p->mouth_h = 3;
    } else if (rec && db > -30) {
        bool beat = (tick / 2) % 2;
        p->lift = beat ? 2 : 0;
        p->left_up = beat;
        p->right_up = !beat;
        p->mouth_h = db > -12 ? 4 : (db > -20 ? 3 : 2);
    } else if (!rec && idle_ticks > SLEEP_AFTER_TICKS) {
        p->eyes = EYES_CLOSED;
        p->lift = (tick / 10) % 2;  // slow breathing
        p->zzz = true;
        p->zzz_phase = tick % 16;
    } else {
        p->lift = (tick / 5) % 2;
        p->mouth_h = rec ? 1 : 0;
    }
}

static void display_task(void *arg)
{
    uint64_t free_bytes = 0;
    int last_marks = 0;
    TickType_t mark_until = 0;
    bool was_rec = false;
    uint32_t idle_ticks = 0;
    for (uint32_t tick = 0;; tick++) {
        if (tick % (2000 / REFRESH_MS) == 0) {
            free_bytes = storage_free_bytes();
        }
        int marks = recorder_mark_count();
        if (marks > last_marks) {
            mark_until = xTaskGetTickCount() + pdMS_TO_TICKS(MARK_SHOW_MS);
        }
        last_marks = marks;
        bool show_mark = (int32_t)(mark_until - xTaskGetTickCount()) > 0;

        bool rec = recorder_is_recording();
        idle_ticks = (rec || rec != was_rec) ? 0 : idle_ticks + 1;
        was_rec = rec;
        clawd_pose_t pose;
        pick_pose(&pose, tick, idle_ticks, rec, show_mark);

        render(free_bytes, show_mark, (tick / 5) % 2 == 0, &pose);
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb);
        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
}

esp_err_t display_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = CONFIG_RP_OLED_SDA_GPIO,
        .scl_io_num = CONFIG_RP_OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  // most modules also have their own pull-ups
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");

    uint8_t addr = 0;
    if (i2c_master_probe(bus, 0x3C, 50) == ESP_OK) {
        addr = 0x3C;
    } else if (i2c_master_probe(bus, 0x3D, 50) == ESP_OK) {
        addr = 0x3D;
    }
    if (!addr) {
        ESP_LOGW(TAG, "No OLED on SDA=GPIO%d SCL=GPIO%d, continuing without display", CONFIG_RP_OLED_SDA_GPIO,
                 CONFIG_RP_OLED_SCL_GPIO);
        i2c_del_master_bus(bus);
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = addr,
        .scl_speed_hz = 400 * 1000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .transaction_timeout_ms = 100,  // don't hang if the display is unplugged
    };
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_ssd1306_config_t ssd_cfg = {.height = HEIGHT};
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(io, &panel_cfg, &s_panel), TAG, "ssd1306");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
#if CONFIG_RP_OLED_FLIP
    esp_lcd_panel_mirror(s_panel, true, true);
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "on");

    ESP_LOGI(TAG, "OLED found at 0x%02X", addr);
    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 3, NULL, 0);
    return ESP_OK;
}
