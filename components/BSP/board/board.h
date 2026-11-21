#pragma once

/**
 * @file  board.h
 * @brief 板级引脚定义（ESP32-C3 Super Mini + Si4732-A10）
 *
 * 本文件只存放硬件引脚映射，不包含任何初始化逻辑。
 * 具体外设驱动（I2C 设备、OLED、编码器、RCLK 等）由其他组件实现。
 *
 * 引脚分配（按需求 1~9）：
 *   1  SDA     I2C 数据线（Si4732 + OLED 共用）
 *   2  SCL     I2C 时钟线（Si4732 + OLED 共用）
 *   3  INT     Si4732 GPO1，调谐完成通知
 *   4  RST     Si4732 复位，GPIO 直控
 *   5  RCLK    Si4732 参考时钟，LEDC 输出 32.768 kHz
 *   6  A       EC11 编码器 A 相
 *   7  B       EC11 编码器 B 相
 *   8  LED     板载 LED
 *   9  KEY     EC11 编码器按键（同时是 BOOT 脚）
 */

#include "driver/gpio.h"
#include "driver/i2c_types.h"

/* ============================ I2C 总线 ============================ */
/* Si4732（7 位 0x11）与 OLED（0x3C/0x3D）共用同一条总线，靠从机地址区分 */
#define BOARD_I2C_PORT          I2C_NUM_0
#define BOARD_I2C_SDA_GPIO      GPIO_NUM_1
#define BOARD_I2C_SCL_GPIO      GPIO_NUM_2

/* I2C 时钟：Si4732 与 SSD1306 规格上限都是 400kHz
 * 400kHz 需要 SDA/SCL 各有 4.7k 外部上拉；长导线不稳定时再降到 100kHz */
#define BOARD_I2C_CLK_HZ        (400 * 1000)

/* ============================ Si4732 ============================ */
#define BOARD_SI4732_GPO1_GPIO  GPIO_NUM_3      /* 调谐完成中断（下降沿） */
#define BOARD_SI4732_RST_GPIO   GPIO_NUM_4      /* 复位，高电平正常工作 */
#define BOARD_SI4732_RCLK_GPIO  GPIO_NUM_5      /* 32.768 kHz 参考时钟输出 */

/* ============================ EC11 编码器 ============================ */
#define BOARD_ENC_A_GPIO        GPIO_NUM_6
#define BOARD_ENC_B_GPIO        GPIO_NUM_7
#define BOARD_ENC_SW_GPIO       GPIO_NUM_9      /* 编码器按键，低电平按下 */

/* ============================ LED ============================ */
#define BOARD_LED_GPIO          GPIO_NUM_8      /* 板载 LED */

/* ============================ 需要避开的引脚 ============================ */
/* IO0    BOOT 按键（拉低进下载模式）
 * IO9    BOOT / strapping，本工程用作编码器按键，上电瞬间不要按住
 * IO11   VDD_SPI（Flash 供电），不可当普通 IO
 * IO18   USB_D-，下载与日志靠它
 * IO19   USB_D+
 * IO20   UART0 RX（默认串口），保留
 * IO21   UART0 TX（默认串口），保留
 */
