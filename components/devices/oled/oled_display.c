#include "oled_display.h"

#include <stdbool.h>

#include "board.h"
#include "board_i2c.h"
#include "esp32_hw_i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "OLED";

static u8g2_t s_u8g2;
static u8g2_esp32_i2c_ctx_t s_ctx;
static bool s_ready = false;

esp_err_t oled_init(uint8_t addr)
{
    i2c_master_bus_handle_t bus = NULL;

    if (s_ready) {
        return ESP_OK;
    }

    /* 复用 i2cdev 建立的总线，避免在同一个 I2C 控制器上重复创建总线 */
    ESP_RETURN_ON_ERROR(board_i2c_get_bus(&bus), TAG, "获取 I2C 总线失败");

    s_ctx.cfg = (u8g2_esp32_i2c_config_t)U8G2_ESP32_I2C_CONFIG_DEFAULT();
    s_ctx.cfg.i2c_port = BOARD_I2C_PORT;
    s_ctx.cfg.sda_pin = BOARD_I2C_SDA_GPIO;
    s_ctx.cfg.scl_pin = BOARD_I2C_SCL_GPIO;
    s_ctx.cfg.clk_hz = BOARD_I2C_CLK_HZ;
    s_ctx.cfg.dev_addr_7bit = addr;
    s_ctx.cfg.reset_pin = U8G2_ESP32_PIN_UNUSED;
    s_ctx.bus_handle = bus;
    s_ctx.initialized = 1;      /* 标记总线已就绪，u8g2 端口层不再自行建总线 */

    ESP_RETURN_ON_ERROR(u8g2_esp32_i2c_set_default_context(&s_ctx), TAG, "设置 u8g2 I2C 上下文失败");

    /* SSD1315 兼容 SSD1306 指令集，0.96 寸面板分辨率为 128x64 */
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_u8g2, U8G2_R0,
                                           u8x8_byte_esp32_hw_i2c,
                                           u8x8_gpio_and_delay_esp32_i2c);

    u8x8_SetI2CAddress(&s_u8g2.u8x8, (uint8_t)(addr << 1));   /* u8x8 需要 8 位地址 */

    ESP_RETURN_ON_ERROR(board_i2c_lock(pdMS_TO_TICKS(1000)), TAG, "获取 I2C 锁超时");
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    u8g2_SetContrast(&s_u8g2, OLED_CONTRAST_DEFAULT);
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);
    board_i2c_unlock();

    s_ready = true;
    ESP_LOGI(TAG, "初始化完成，地址 0x%02X，分辨率 %dx%d", addr, OLED_WIDTH, OLED_HEIGHT);
    return ESP_OK;
}

void oled_refresh(void)
{
    if (!s_ready) {
        return;
    }
    if (board_i2c_lock(pdMS_TO_TICKS(1000)) != ESP_OK) {
        return;
    }
    u8g2_SendBuffer(&s_u8g2);
    board_i2c_unlock();
}

u8g2_t *oled_get_u8g2(void)
{
    return s_ready ? &s_u8g2 : NULL;
}
