idf_component_register(
    SRCS "main.c" "settings.c"
    INCLUDE_DIRS "."
    REQUIRES BSP devices radio u8g2 esp_driver_uart nvs_flash
)
