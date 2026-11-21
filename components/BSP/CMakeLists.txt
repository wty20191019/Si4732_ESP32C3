idf_component_register(
    SRC_DIRS "led" "rclk" "encoder"
    INCLUDE_DIRS "board" "led" "rclk" "encoder"
    REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_i2c freertos
)

component_compile_options(-ffast-math -O3)
