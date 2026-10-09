#ifndef __OLED_H
#define __OLED_H

#include "stm32f10x.h"

/*---------------------------------------------------------------------------
  SSD1306 0.96 寸 128x64 OLED 驱动，走 I2C（4 针模块，丝印 GND VCC SCL SDA）

  设计要点：
    1. 绘制只修改 OLED_GRAM[8][128]，刷新时只发送变化的 32 字节块。
       软件缓存用于差分比较；屏幕本身没有原子整帧交换。
    2. 坐标是【像素】：x = 0~127，y = 0~63。
       不要用原厂那种"页号 0~7"的写法，画图时很别扭。
    3. 运行时 StartRefresh/RefreshStep 分步发送，忙时不要重绘显存。
       初始化与校准可用同步 OLED_Refresh；主界面使用异步短块刷新。

  地址：模块上标的 0x78 是【8 位写地址】，7 位地址是 0x3C。
        I2C 层统一用 7 位地址，所以这里是 0x3C。
---------------------------------------------------------------------------*/

#define OLED_ADDR7      0x3C

void OLED_Init(void);
void OLED_Clear(void);
void OLED_Refresh(void);                    /* 把显存推到屏幕，必须调用才可见 */
void OLED_StartRefresh(void);
void OLED_RefreshStep(void);
uint8_t OLED_RefreshBusy(void);
void OLED_SetIdle(uint8_t idle); /* Request gradual dimming; does not turn off. */
void OLED_Service(uint32_t now); /* One ACK-checked contrast/recovery transaction. */
void OLED_ShowSmallString(uint8_t x, uint8_t y, const char *str);
void OLED_ShowSmallStringInverse(uint8_t x, uint8_t y, const char *str);
void OLED_ShowLargeString(uint8_t x, uint8_t y, const char *str); /* 5x7 at 2x scale */

void OLED_DrawPoint(uint8_t x, uint8_t y, uint8_t dot);
void OLED_DrawRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint8_t dot);
void OLED_FillRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint8_t dot);
void OLED_DrawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint8_t dot);
void OLED_DrawCircle(uint8_t xc, uint8_t yc, uint8_t r, uint8_t dot);
void OLED_DrawDisc(uint8_t xc, uint8_t yc, uint8_t r, uint8_t dot);   /* 实心圆 */

void OLED_ShowChar(uint8_t x, uint8_t y, char ch);           /* 8x16 点阵 */
void OLED_ShowString(uint8_t x, uint8_t y, const char *str);
void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t digits);
void OLED_ShowSignedNum(uint8_t x, uint8_t y, int32_t num, uint8_t digits);
void OLED_ShowFixed1(uint8_t x, uint8_t y, int32_t val_x10, uint8_t int_digits);

#endif
