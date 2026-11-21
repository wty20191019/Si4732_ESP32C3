idf_component_register(
    SRC_DIRS "i2c" "oled"
    INCLUDE_DIRS "i2c" "oled"
    REQUIRES BSP esp_driver_i2c u8g2 freertos
    PRIV_REQUIRES i2cdev
)

# 社区版 u8g2（nixy4/u8g2）在 IDF 6 下没有声明 esp_driver_gpio 依赖，
# 导致 esp32_hw_i2c.c / esp32_hw_spi.c 编译时找不到 driver/gpio.h。
# 这里给它的编译目标补上依赖：不改 managed_components，组件重新下载也不会丢。
idf_build_get_property(_all_components BUILD_COMPONENTS)
if("nixy4__u8g2" IN_LIST _all_components)
    idf_component_get_property(_u8g2_lib nixy4__u8g2 COMPONENT_LIB)
    if(TARGET ${_u8g2_lib})
        target_link_libraries(${_u8g2_lib} PRIVATE idf::esp_driver_gpio)
    endif()
endif()
