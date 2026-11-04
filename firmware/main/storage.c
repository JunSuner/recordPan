#include "storage.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

static bool s_mounted;

static void log_space(void)
{
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(STORAGE_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        // 16 kHz / 16-bit WAV = 32000 bytes per second per channel
        ESP_LOGI(TAG, "%s: %llu KB free of %llu KB (~%llu min mono / ~%llu min stereo WAV)", STORAGE_MOUNT_POINT,
                 free_bytes / 1024, total / 1024, free_bytes / 32000 / 60, free_bytes / 64000 / 60);
    }
}

#if CONFIG_RP_STORAGE_FLASH

static wl_handle_t s_wl = WL_INVALID_HANDLE;

esp_err_t storage_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,  // first boot: the partition is blank
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_MOUNT_POINT, "storage", &mount_cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Flash mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    log_space();
    return ESP_OK;
}

#else  // CONFIG_RP_STORAGE_SD

static sdmmc_card_t *s_card;

// After a failed mount, checks whether anything drives MISO: a line that just
// follows the internal pull resistor is floating (unpowered module or broken wire).
static void probe_miso(void)
{
    const gpio_num_t miso = CONFIG_RP_SD_MISO_GPIO;
    const gpio_num_t cs = CONFIG_RP_SD_CS_GPIO;
    gpio_reset_pin(cs);
    gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    gpio_reset_pin(miso);
    gpio_set_direction(miso, GPIO_MODE_INPUT);

    for (int cs_level = 1; cs_level >= 0; cs_level--) {
        gpio_set_level(cs, cs_level);
        gpio_set_pull_mode(miso, GPIO_PULLDOWN_ONLY);
        vTaskDelay(pdMS_TO_TICKS(5));
        int with_pulldown = gpio_get_level(miso);
        gpio_set_pull_mode(miso, GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(5));
        int with_pullup = gpio_get_level(miso);
        ESP_LOGW(TAG, "MISO probe (CS=%d): pulldown->%d pullup->%d => %s", cs_level, with_pulldown, with_pullup,
                 with_pulldown == with_pullup ? "driven" : "floating");
    }
    gpio_reset_pin(cs);
    gpio_reset_pin(miso);
}

// Initializes the SPI bus with the given pins and mounts the card.
// On failure the bus is freed again so the pins can be reused.
static esp_err_t try_mount(int mosi, int sck)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_RP_SD_FREQ_KHZ;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi,
        .miso_io_num = CONFIG_RP_SD_MISO_GPIO,
        .sclk_io_num = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = CONFIG_RP_SD_CS_GPIO;
    slot_cfg.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 32 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(STORAGE_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        spi_bus_free(host.slot);
    }
    return err;
}

esp_err_t storage_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    esp_err_t err = try_mount(CONFIG_RP_SD_MOSI_GPIO, CONFIG_RP_SD_SCK_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s (card inserted? FAT32 formatted? wiring?)", esp_err_to_name(err));
        probe_miso();
        // Swapped MOSI/SCK wires are a common mistake; detect it instead of guessing.
        if (try_mount(CONFIG_RP_SD_SCK_GPIO, CONFIG_RP_SD_MOSI_GPIO) != ESP_OK) {
            return err;
        }
        ESP_LOGW(TAG, "Card works with MOSI/SCK swapped (MOSI on GPIO%d, SCK on GPIO%d): please swap the wires",
                 CONFIG_RP_SD_SCK_GPIO, CONFIG_RP_SD_MOSI_GPIO);
    }
    sdmmc_card_print_info(stdout, s_card);
    s_mounted = true;
    log_space();
    return ESP_OK;
}

#endif  // CONFIG_RP_STORAGE_FLASH

uint64_t storage_free_bytes(void)
{
    uint64_t total = 0, free_bytes = 0;
    if (!s_mounted || esp_vfs_fat_info(STORAGE_MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
        return 0;
    }
    return free_bytes;
}

bool storage_is_mounted(void)
{
    return s_mounted;
}

esp_err_t storage_next_rec_base(char *out, size_t len)
{
    DIR *dir = opendir(STORAGE_MOUNT_POINT);
    if (!dir) {
        return ESP_FAIL;
    }
    int max_index = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int index;
        if (strncasecmp(entry->d_name, "REC_", 4) == 0 && sscanf(entry->d_name + 4, "%d", &index) == 1) {
            if (index > max_index) {
                max_index = index;
            }
        }
    }
    closedir(dir);
    snprintf(out, len, STORAGE_MOUNT_POINT "/REC_%04d", max_index + 1);
    return ESP_OK;
}
