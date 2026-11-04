idf_component_register(
    SRCS "main.c" "led.c" "buttons.c" "storage.c" "wav_writer.c" "ogg_opus.c" "recorder.c" "display.c" "buzzer.c" "ble_service.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_sdspi esp_driver_spi esp_driver_i2c esp_lcd bt nvs_flash
                  fatfs sdmmc wear_levelling esp_ringbuf esp_timer
)
