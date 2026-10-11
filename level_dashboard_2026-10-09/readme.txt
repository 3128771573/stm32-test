STM32 多功能小仪表 · E 版工程
================================

版本：V2026.10.10E（屏显 20261010E）
硬件：STM32F103C8T6、MPU6050、4 针 I2C OLED、DHT11、W25QXX

快速开始
--------
1. 先看 docs/wiring-beginner.md，断电接线，所有模块共地并使用 3.3V。
2. 用 ST-Link 烧录 firmware/level.hex；校验值见 firmware/SHA256SUMS.txt。
3. 上电查看 POST，自检后平放设备等待校准，再转动编码器翻页。
4. Keil 打开 MDK-ARM/level.uvprojx 按 F7，或在本目录执行 .\build.cmd。

当前发布构建：Keil ARM Compiler 5.06 update 7，0 错误、0 警告；
ROM 31,536 B，RW+ZI 8,048 B。E 版新增会话记录仍需烧板验收。

页面
----
LEVEL 水平测量；CLIMATE 温湿度；HISTORY 测量历史；SYSTEM 设备状态；
RAW DATA 原始值；TREND 变化曲线；DEBUG 运行诊断；STROBE 红蓝灯效；
RUNTIME 本次开机时长；SESSIONS 历史开机记录。

引脚速查
--------
OLED/MPU：PB6=SCL、PB7=SDA；蜂鸣器：PB8；DHT11 DATA：PA1；
W25QXX：PA4=CS、PA5=CLK、PA6=MISO、PA7=MOSI；
编码器 A/C/B：PB0/GND/PB1；RGB LED：PA8/PA9/PA10，各串 330Ω；
可选按钮：PB10 接 GND。完整步骤见 docs/wiring-beginner.md。

数据与安全
----------
Flash 末尾 64KB 用于记录，其中前 8KB 为会话事件区。不要全片擦除。
突然断电不能保存准确关机时刻，历史显示最近一分钟检查点形成的时长下界；
本功能不依赖 RTC、日期或 VBAT。所有模块按 3.3V 逻辑连接。

文档：docs/dashboard.md（页面） docs/sessions.md（会话）
      docs/strobe.md（灯效） docs/validation.md（验收）
      docs/STM32多功能小仪表-固件技术白皮书-V2026.10.10E-修复版.pdf
