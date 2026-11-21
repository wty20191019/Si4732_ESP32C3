#pragma once

/**
 * @file  board_i2c.h
 * @brief 板级 I2C 总线管理（多设备共用一条总线）
 *
 * 总线由 esp-idf-lib 的 i2cdev 统一创建，MPU6500 / AHT20 / BMP280 / OLED
 * 都挂在这条总线上，通过不同从机地址区分。OLED（u8g2）通过
 * board_i2c_get_bus() 取得同一条总线句柄，不会重复创建控制器。
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2C 总线（内部调用 i2cdev_init()，可重复调用）
 *
 * @return ESP_OK 成功，其他为 esp_err_t 错误码
 */
esp_err_t board_i2c_init(void);

/**
 * @brief 获取全局 I2C 互斥锁（递归锁，自带优先级继承）
 *
 * 所有总线操作都要先拿这把锁，避免多任务同时访问同一条总线。
 * 注意：AHT20 这类需要等待转换时间的操作，应在等待前释放锁。
 *
 * @param timeout 等待时间，可用 pdMS_TO_TICKS() 或 portMAX_DELAY
 * @return ESP_OK 成功，ESP_ERR_TIMEOUT 超时
 */
esp_err_t board_i2c_lock(TickType_t timeout);

/**
 * @brief 释放全局 I2C 互斥锁（与 board_i2c_lock 成对调用）
 */
void board_i2c_unlock(void);

/**
 * @brief 获取 i2cdev 创建的共享总线句柄，供 u8g2 等第三方组件使用
 *
 * @param[out] bus 返回的总线句柄
 * @return ESP_OK 成功
 */
esp_err_t board_i2c_get_bus(i2c_master_bus_handle_t *bus);

/**
 * @brief 探测指定 7 位地址是否有从机应答
 *
 * @return true 有 ACK
 */
bool board_i2c_probe_addr(uint8_t addr);

/**
 * @brief 复位 I2C 总线（拉 9 个 SCL 时钟 + STOP）
 *
 * 从机异常把 SDA 拉住时，用它释放总线，再重新初始化设备。
 */
esp_err_t board_i2c_bus_reset(void);

/**
 * @brief 检查 SDA/SCL 空闲电平是否都为高
 *
 * 总线被拉低或短路时返回 false，可用于在扫描/通信前快速判断总线状态。
 */
bool board_i2c_lines_idle(void);

/**
 * @brief 扫描 0x08~0x77 地址空间，找出所有应答的从机
 *
 * @param[out] out_addrs 存放扫描到的 7 位地址，可为 NULL
 * @param[in]  max_addrs out_addrs 的容量
 * @return 扫描到的设备数量（可能大于 max_addrs）
 */
size_t board_i2c_scan(uint8_t *out_addrs, size_t max_addrs);

#ifdef __cplusplus
}
#endif
