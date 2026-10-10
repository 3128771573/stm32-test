<div align="center">

<img src="docs/hero.svg" alt="STM32 多功能小仪表" width="100%">

<h1>STM32 多功能小仪表</h1>

**STM32F103C8T6 · MPU6050 · SSD1306 · DHT11 · W25QXX**

[烧录固件](firmware/level.hex) · [从零接线](docs/wiring-beginner.md) · [页面与验收](docs/dashboard.md) · [更新记录](CHANGELOG.md)

![Keil](https://img.shields.io/badge/Keil-AC5-334155?style=flat-square)
![Build](https://img.shields.io/badge/Build-0_errors%20%7C%200_warnings-16a34a?style=flat-square)
![Hardware](https://img.shields.io/badge/E版-待实物验收-f59e0b?style=flat-square)

</div>

## 硬件与灯效预览

<div align="center">

<img src="docs/strobe.png" alt="STROBE 红蓝灯效页面" width="86%">

红蓝灯效独立页面 · 支持五种节奏模式

</div>

当前发布为 **2026-10-10 E**（屏显版本 `20261010E`）。固件由 Keil ARM Compiler 5.06 update 7 构建，编译无错误、无警告。原七阶段水平仪有实物验收记录；E 版新增的历史开机时长功能仍待烧板验收。

## 能做什么

| 功能 | 内容 |
|---|---|
| 倾斜仪表 | 平滑气泡、稳定角度、倒置与大倾角保护、偏移联动 RGB 灯 |
| 温湿度 | DHT11 周期采样、超时恢复与状态诊断 |
| 数据记录 | W25QXX 保存测量历史，掉电保留并循环使用末尾 64KB |
| 可观测性 | 原始六轴数据、约 12 秒趋势、传感器和调度诊断 |
| 灯效 | STROBE 独立页面，旋转选择 OFF、ALT、DOUBLE、TRIPLE、CHASE |
| 开机记录 | RUNTIME 查看本次时长；SESSIONS 浏览历史开机时长与复位原因 |
| 启动检查 | 逐项显示真实自检结果；只有真实读回或检查完成后才显示完成 |

旋转编码器负责翻页和选择。手上的 A/C/B 模块没有按压引脚，PB10 可以空着，也可外接独立按钮。

## 下载与烧录

- 最新固件：[level.hex](firmware/level.hex)
- 上一公开七页版：[level_previous.hex](firmware/level_previous.hex)
- 校验值：[SHA256SUMS.txt](firmware/SHA256SUMS.txt)

使用 ST-Link 烧录 `level.hex`。已有 Flash 测量记录区仍保留在芯片末尾 64KB；升级无需清空。遇到未知 Flash 内容时程序会锁定记录写入，不会自动格式化或全片擦除。

## 接线与上手

建议按以下顺序操作：

1. 阅读[一步一步接线教程](docs/wiring-beginner.md)，先接 3.3V、GND 和编码器，确认旋转翻页。
2. 逐个增加 LED、电阻、DHT11 和 W25QXX；每一步都按教程中的可观测结果验收。
3. 查看[引脚表和页面操作](docs/dashboard.md)，放平上电并完成校准。
4. 遇到问题时查[验证与排障记录](docs/validation.md)。

![扩展模块接线图](docs/wiring.png)

> 所有模块共地并使用 3.3V 逻辑。红、绿、蓝 LED 每颗各串 330Ω；DHT11 裸传感器 DATA 需 4.7kΩ 上拉，三针小板通常已带上拉。详细接法以教程为准。

## 引脚速查

| 模块 / 信号 | STM32 引脚 |
|---|---|
| OLED + MPU6050 软件 I2C | PB6=SCL，PB7=SDA |
| 蜂鸣器 | PB8（低电平触发） |
| DHT11 DATA | PA1 |
| W25QXX SPI1 | PA4=CS，PA5=CLK，PA6=MISO，PA7=MOSI |
| 编码器 | PB0=A，GND=C，PB1=B |
| RGB 红 / 绿 / 蓝 | PA8 / PA9 / PA10，每路串 330Ω |
| 可选独立按钮 | PB10 与 GND；没有按钮可悬空 |
| ST-Link SWD | PA13=SWDIO，PA14=SWCLK |

## 开机时长记录的边界

`RUNTIME` 是本次上电后的实时计时；Flash 每分钟写入一次存活检查点，`SESSIONS` 可查看过往开机记录。突然断电时 MCU 无法记录准确断电时刻，因此显示最近检查点对应的运行时长下界；没有检查点时显示 `NO CHECKPOINT`。该功能不依赖 RTC、日期或 VBAT 电池。旧记录容量有限，新记录会逐步覆盖更早内容，详见[会话记录说明](docs/sessions.md)。

## 编译

需要 Windows、Keil MDK 和 ARM Compiler 5.06 update 7。打开 `MDK-ARM/level.uvprojx` 后按 F7 编译，或在本目录运行：

```powershell
.\build.cmd
```

脚本会自动查找 Keil UV4。构建结果：ROM 31,536 字节；RAM（RW + ZI）8,048 字节；0 错误、0 警告。源码为 GBK 编码，Markdown 为 UTF-8；不要用会改写编码的工具批量保存 `.c/.h`。

## 文档导航

| 文档 | 内容 |
|---|---|
| [新手接线教程](docs/wiring-beginner.md) | 面包板、电源轨、电阻、模块逐步接法 |
| [接线、页面与验收](docs/dashboard.md) | 完整引脚表、旋转操作、每页行为 |
| [开机时长与历史会话](docs/sessions.md) | 记录格式、检查点和掉电误差边界 |
| [红蓝灯效](docs/strobe.md) | 模式说明与操作 |
| [软件验证记录](docs/validation.md) | 已验证项目、实机验收清单和限制 |
| [更新日志](CHANGELOG.md) | 版本功能变化 |

## 硬件验收状态

DHT11 的 EXTI 初始化问题已经修复并由用户确认恢复读取。其余新版软件已完成 Keil 构建和逻辑检查；新会话记录、OLED 恢复、灯效手感等仍须在真实硬件上验收。请按[验证记录](docs/validation.md)逐项确认，软件构建结果不代替实机结果。