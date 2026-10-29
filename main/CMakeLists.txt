idf_component_register(
    SRCS "main.c" "launcher.c"
    INCLUDE_DIRS "."
    REQUIRES smartring_plus esp_lvgl_adapter lvgl
)
