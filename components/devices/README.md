# devices 组件（外设驱动）

| 目录 | 内容 |
|---|---|
| `i2c` | 共用 I2C 总线（i2cdev 创建）+ 全局递归互斥锁 + 地址探测 |
| `oled` | 0.96 寸 OLED（SSD1306/SSD1315，128x64），显示库使用 u8g2 |

## I2C 总线

- Si4732（7 位地址 0x11，见 `components/radio`）与 OLED（0x3C/0x3D）共用一条总线
- 总线由 `esp-idf-lib/i2cdev` 创建，所有访问都要用 `board_i2c_lock()` / `board_i2c_unlock()` 串行化
- `board_i2c_get_bus()` 把同一条总线句柄交给 u8g2，避免重复创建 I2C 控制器
- `board_i2c_probe_addr()` 探测单个 7 位地址，用于上电诊断接线

## OLED

- 分辨率 128x64，u8g2 驱动 `ssd1306_i2c_128x64_noname_f`
- `oled_get_u8g2()` 返回底层上下文，可用完整 u8g2 绘图 API
- `oled_refresh()` 把缓冲区内容发送到屏幕

依赖由 `idf_component.yml` 声明（`esp-idf-lib/i2cdev`、`nixy4/u8g2`），首次构建时由组件管理器自动下载。
