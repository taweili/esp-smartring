# ringkit — generic Smart Ring Plus skeleton.
#
# Wraps the whole viewesmart/smartring_plus BSP so applications never touch
# bsp_* directly. smartring_plus is a PUBLIC requirement: it already exports
# lvgl, esp_lcd_touch, esp_driver_i2c, esp_driver_i2s, espressif__esp_codec_dev
# and sdmmc, all of which appear in ringkit's public headers and therefore
# propagate to dependents.
idf_component_register(
    SRCS
        "src/ring_board.c"
        "src/ring_display.c"
        "src/ring_touch.c"
        "src/ring_battery.c"
        "src/ring_power.c"
        "src/ring_audio.c"
        "src/ring_imu.c"
        "src/ring_sd.c"
        "src/ring_wifi.c"
        "src/ring_ui.c"
        "src/ring_rand.c"
    INCLUDE_DIRS "include"
    REQUIRES smartring_plus lvgl esp_lvgl_adapter esp_wifi esp_netif esp_event nvs_flash
)
