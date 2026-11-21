# Si4732 收音机（ESP32-C3 · ESP-IDF）

基于 **ESP-IDF 6.1 / ESP32-C3** 的 Si4732-A10 收音机工程骨架。
工程结构从 ESP32-C3 传感器采集模板复制而来，已裁掉 IMU / 温湿度 / 气压计 /
WiFi 网页 / CSV 日志等模块，替换为 Si4732 收音链路。

> 当前阶段：**骨架 + 基础收音驱动**。
> Si4732 驱动（复位、上电、REFCLK、调谐、搜台、音量、状态读取）已实现；
> 应用层（OLED 页面、编码器交互、串口命令）是可用但精简的骨架，
> 后续按“待办路线”继续补功能。

## 硬件平台

- **芯片**：ESP32-C3（RISC-V 单核，4MB Flash）
- **开发板**：ESP32-C3 Super Mini（原生 USB，下载走 GPIO18/19）
- **收音芯片**：Si4732-A10（FM / AM / SW / LW，I2C 地址 0x22）
- **显示**：0.96 寸 OLED（SSD1306/SSD1315，128×64，I2C 0x3C）
- **输入**：EC11 旋转编码器（A/B 相 + 按键）
- **音频**：Si4732 LOUT/ROUT → PAM8403 → 4Ω 3W 扬声器

### 引脚分配

| 编号 | ESP32-C3 | 连接 | 说明 |
|---|---|---|---|
| 1 | GPIO1 | Si4732 SDIO + OLED SDA | I2C 数据线，4.7k 上拉到 3V3 |
| 2 | GPIO2 | Si4732 SCLK + OLED SCL | I2C 时钟线，4.7k 上拉到 3V3 |
| 3 | GPIO3 | Si4732 GPO1 | 调谐完成中断，内部上拉（当前用轮询，预留） |
| 4 | GPIO4 | Si4732 RST | 复位，高电平正常工作 |
| 5 | GPIO5 | Si4732 RCLK | LEDC 输出 32.768 kHz |
| 6 | GPIO6 | EC11 A | 内部上拉 |
| 7 | GPIO7 | EC11 B | 内部上拉 |
| 8 | GPIO8 | 板载 LED | 状态指示 |
| 9 | GPIO9 | EC11 按键 | BOOT 脚，内部上拉，上电瞬间不要按住 |

I2C 速率默认 **400 kHz**（`components/BSP/board/board.h` 中 `BOARD_I2C_CLK_HZ`）。
长导线或没有外部上拉时改到 100 kHz。

### 不能省的硬件

- SDA/SCL 各一个 4.7 kΩ 上拉到 3V3（内部上拉太弱）
- FMI 串联 1 nF C0G 耦合电容
- RST 不能悬空
- SENB 接地（锁定 0x22）
- VDD 只能接 3.3 V

## 目录结构

```
Si4732-A10/
├─ CMakeLists.txt              # project(Si4732_Radio)
├─ sdkconfig.defaults          # 目标芯片/Flash/分区/控制台/u8g2
├─ partitions-4MiB.csv         # 4MB Flash：应用分区 3MB
├─ main/
│  └─ main.c                   # app_main + radio/ui/console/led 任务
├─ components/
│  ├─ BSP/                     # 板级支持
│  │  ├─ board/board.h         # 引脚映射（唯一改引脚的地方）
│  │  ├─ led/                  # LED
│  │  ├─ rclk/                 # LEDC 32.768 kHz 参考时钟
│  │  └─ encoder/              # EC11 正交解码 + 按键长短按
│  ├─ devices/                 # 外设驱动
│  │  ├─ i2c/                  # 共用 I2C 总线 + 互斥锁 + 扫描
│  │  └─ oled/                 # 128×64 OLED（u8g2）
│  ├─ radio/
│  │  └─ si4732/               # Si4732 驱动（AN332 命令层）
│  ├─ filters/                 # 通用二阶低通（预留给 RSSI 平滑）
│  └─ ui/                      # 通用步进选择器（预留）
├─ tools/gen_cpp_properties.ps1
└─ .vscode/                    # IntelliSense 配置
```

## 软件架构

| 任务 | 优先级 | 职责 |
|---|---|---|
| `radio` | 6 | Si4732 初始化/重试、编码器调谐或音量、波段切换、500ms 状态轮询 |
| `encoder`（BSP 内部） | 5 | 2ms 扫描 A/B 相与按键，2ms 内消抖、长短按判定 |
| `ui` | 3 | 状态变化时刷新 OLED（3 秒重试初始化） |
| `console` | 2 | UART0 读取调试命令 |
| `led` | 1 | 500ms 闪烁 |

关键约束：

- **I2C 互斥**：Si4732 与 OLED 共用总线，驱动内部统一用 `board_i2c_lock()`
- **阻塞操作在任务里**：调谐/搜台最长等待 2 秒，但只阻塞 `radio` 任务
- **参考时钟先行**：`rclk_start()` → `board_i2c_init()` → `si4732_init()`
- **不启用 WiFi/BLE**：避免射频噪声串入接收链路

## 编译与烧录

```powershell
idf.py set-target esp32c3     # 首次
idf.py build
idf.py -p COM20 flash monitor # Ctrl+] 退出
```

首次构建会自动从组件仓库下载 `esp-idf-lib/i2cdev` 与 `nixy4/u8g2`。

分区表（`partitions-4MiB.csv`）：

| 分区 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24KB |
| phy_init | data/phy | 0xf000 | 4KB |
| factory | app | 0x10000 | 3MB |

### 串口命令（UART0，115200）

| 命令 | 功能 |
|---|---|
| `f101100` | FM 101.100 MHz（也可写 `f101`） |
| `a1000` | 中波 1000 kHz |
| `s9700` | 短波 9700 kHz |
| `l200` | 长波 200 kHz |
| `fm` / `am` / `sw` / `lw` | 切换波段 |
| `v` | 打印频率 / RSSI / SNR / 音量 |
| `vol40` | 音量 0~63 |
| `+` / `-` | 向上 / 向下搜台 |
| `h` | 帮助 |

> 控制台主输出是 UART0（GPIO20/21），USB Serial/JTAG 作为副控制台。
> 用 USB 线看日志没问题，但**串口输入只从 UART0 读取**。

### 编码器操作

- 旋转：调谐（步进按波段：FM 100 kHz、MW 10 kHz、SW 5 kHz、LW 3 kHz）或调音量
- 短按：切换波段 FM → AM → SW → LW
- 长按（1 秒）：切换“调谐 / 音量”模式，OLED 第三行显示 `TUNE` 或 `VOL`

## Si4732 驱动 API（`components/radio/si4732/si4732.h`）

```c
si4732_init();                 // 复位 + 上电 FM + REFCLK + 音量
si4732_set_band(SI4732_BAND_SW);
si4732_tune(9700);             // kHz
si4732_seek(true);             // 向上搜台
si4732_set_volume(40);         // 0~63
si4732_get_status(&status);    // 频率 / RSSI / SNR / valid
si4732_set_property(prop, v);  // AN332 通用属性
```

实现要点：

1. 每条命令前/后都轮询 STATUS 的 CTS 位（bit7），ERR 位（bit6）置位时报错
2. `POWER_UP` 参数：FM 用 `FUNC=0`，AM/SW/LW 用 `FUNC=1`，OPMODE=0x05（模拟输出）
3. 上电后设置 `REFCLK_FREQ=32768`、`REFCLK_PRESCALE=1`，否则频率整体偏移
4. FM 与 AM 模式切换需要重新 `POWER_UP`；MW/SW/LW 之间只改频率
5. TUNE_STATUS 的 READFREQ：FM 单位 10 kHz，AM 单位 kHz

## 待办路线（骨架之后）

- [ ] GPO1 下降沿中断 + 信号量替代 TUNE_STATUS 轮询（降低调谐期间的 I2C 占用）
- [ ] OLED 多页面：主界面 / 信号质量 / 版本信息 / 音量条
- [ ] 频率收藏与掉电保存（NVS：频率、波段、音量、静音状态）
- [ ] FM 立体声指示（解析 `FM_RSQ_STATUS` 的 STEREO 位）
- [ ] RDS：`FM_RDS_STATUS` 读取电台名 / 节目类型
- [ ] RSSI/SNR 二阶低通平滑（复用 `components/filters/biquad_lpf`）
- [ ] 编码器加速（快速旋转时步进加倍）
- [ ] 电池供电与低功耗

## 调试顺序

1. 上电看日志：`I2C 扫描` 应出现 `0x22`（Si4732）和 `0x3C`（OLED）
2. 若没有 0x22：查 SENB 是否接地、RST 是否被拉低、SDA/SCL 上拉
3. 若频率偏移：确认 GPIO5 有 32.768 kHz 波形、`REFCLK_FREQ` 设置为 32768
4. 串口发 `f101100`，听扬声器是否有声音
5. 切 `am` + `a1000` 测磁棒天线
6. 最后再接编码器和 OLED 排查交互

## 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| I2C 扫描不到 0x22 | SENB 未接地、RST 悬空、上拉缺失、模块没供电 |
| 频率整体偏移 | RCLK 没输出或 `REFCLK_FREQ` 没设成 32768 |
| 调谐总超时 | 芯片未正常上电（检查 POWER_UP 后的 CTS），或天线/波段不匹配 |
| OLED 花屏 | I2C 速率过高，把 `BOARD_I2C_CLK_HZ` 降到 400k 以下 |
| 上电直接进下载模式 | GPIO9（BOOT/编码器按键）上电瞬间被按住 |
| 串口命令没反应 | 命令走 UART0（GPIO20/21），不是 USB |
