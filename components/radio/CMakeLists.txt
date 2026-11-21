idf_component_register(
    SRC_DIRS "si4732"
    INCLUDE_DIRS "si4732"
    REQUIRES BSP devices esp_driver_i2c esp_driver_gpio freertos
    PRIV_REQUIRES esp_timer
)
