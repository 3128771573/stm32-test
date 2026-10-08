#include "oled.h"
#include "oledfont.h"
#include "i2c.h"

/*---------------------------------------------------------------------------
  显存：8 页 x 128 列。
  OLED_GRAM[页][列]，字节的 bit0 是那一页最上面一行，bit7 是最下面一行。
  这个位序必须和字库一致（字库里每个字节的 bit0 也是最上面一行），
  否则整屏的字会上下镜像。
---------------------------------------------------------------------------*/
static uint8_t OLED_GRAM[8][128];

/* 写一条命令：控制字节 0x00 之后跟命令 */
static void oled_write_cmd(uint8_t cmd)
{
	SoftI2C_Begin(OLED_ADDR7);
	SoftI2C_SendByte(0x00);
	SoftI2C_SendByte(cmd);
	SoftI2C_End();
}

void OLED_Init(void)
{
	oled_write_cmd(0xAE);                           /* 关显示 */
	oled_write_cmd(0xD5); oled_write_cmd(0x80);     /* 显示时钟分频 */
	oled_write_cmd(0xA8); oled_write_cmd(0x3F);     /* 多路复用比 1/64 */
	oled_write_cmd(0xD3); oled_write_cmd(0x00);     /* 显示偏移 0 */
	oled_write_cmd(0x40);                           /* 显示起始行 0 */
	oled_write_cmd(0x20); oled_write_cmd(0x00);     /* 寻址方式 = 水平寻址，
	                                                   这样 1024 字节能一次写完 */
	oled_write_cmd(0xA1);                           /* 左右方向正常 */
	oled_write_cmd(0xC8);                           /* 上下方向正常 */
	oled_write_cmd(0xDA); oled_write_cmd(0x12);     /* COM 引脚硬件配置 */
	oled_write_cmd(0x81); oled_write_cmd(0xCF);     /* 对比度 */
	oled_write_cmd(0xD9); oled_write_cmd(0xF1);     /* 预充电周期 */
	oled_write_cmd(0xDB); oled_write_cmd(0x30);     /* VCOMH
	                                                   注意中景园 I2C 版是 0x30，
                                                   SPI 例程里那个 0x40 不能照抄 */
	oled_write_cmd(0xA4);                           /* 显示内容跟随显存 */
	oled_write_cmd(0xA6);                           /* 正常显示，不反白 */
	oled_write_cmd(0x8D); oled_write_cmd(0x14);     /* 电荷泵开 */
	oled_write_cmd(0xAF);                           /* 开显示 */

	OLED_Clear();
	OLED_Refresh();
}

void OLED_Clear(void)
{
	uint8_t page, col;

	for (page = 0; page < 8; page++)
	{
		for (col = 0; col < 128; col++)
		{
			OLED_GRAM[page][col] = 0x00;
		}
	}
}

/*---------------------------------------------------------------------------
  整屏刷新：一个 I2C 事务连续发 1025 个字节（1 个控制字节 + 1024 个数据）。
  这就是"整屏只在刷完之后才变"的原因，中间过程屏上根本看不到。
---------------------------------------------------------------------------*/
void OLED_Refresh(void)
{
	uint8_t page, col;

	oled_write_cmd(0x20); oled_write_cmd(0x00);                         /* 水平寻址 */
	oled_write_cmd(0x21); oled_write_cmd(0x00); oled_write_cmd(0x7F);   /* 列 0~127 */
	oled_write_cmd(0x22); oled_write_cmd(0x00); oled_write_cmd(0x07);   /* 页 0~7 */

	SoftI2C_Begin(OLED_ADDR7);
	SoftI2C_SendByte(0x40);                     /* 控制字节：后面全是数据 */
	for (page = 0; page < 8; page++)
	{
		for (col = 0; col < 128; col++)
		{
			SoftI2C_SendByte(OLED_GRAM[page][col]);
		}
	}
	SoftI2C_End();
}

void OLED_DrawPoint(uint8_t x, uint8_t y, uint8_t dot)
{
	uint8_t page, bit;

	if (x > 127 || y > 63) return;      /* 越界直接丢，画圆时会有负坐标绕回来 */

	page = (uint8_t)(y >> 3);           /* y / 8，第几页 */
	bit  = (uint8_t)(y & 0x07);         /* y % 8，页内第几行 */

	if (dot)
		OLED_GRAM[page][x] |= (uint8_t)(1 << bit);
	else
		OLED_GRAM[page][x] &= (uint8_t)~(1 << bit);
}

void OLED_DrawRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint8_t dot)
{
	uint16_t i;                         /* 用 16 位，避免 x1 = 127 时 uint8_t 死循环 */

	for (i = x0; i <= x1; i++)
	{
		OLED_DrawPoint((uint8_t)i, y0, dot);
		OLED_DrawPoint((uint8_t)i, y1, dot);
	}
	for (i = y0; i <= y1; i++)
	{
		OLED_DrawPoint(x0, (uint8_t)i, dot);
		OLED_DrawPoint(x1, (uint8_t)i, dot);
	}
}

void OLED_FillRect(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint8_t dot)
{
	uint16_t x, y;

	for (x = x0; x <= x1; x++)
	{
		for (y = y0; y <= y1; y++)
		{
			OLED_DrawPoint((uint8_t)x, (uint8_t)y, dot);
		}
	}
}

/* Bresenham 直线 */
void OLED_DrawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint8_t dot)
{
	int16_t x = x0, y = y0;
	int16_t dx, dy, sx, sy, err, e2;

	dx = (x1 > x0) ? (int16_t)(x1 - x0) : (int16_t)(x0 - x1);
	dy = (y1 > y0) ? (int16_t)(y1 - y0) : (int16_t)(y0 - y1);
	sx = (x0 < x1) ? 1 : -1;
	sy = (y0 < y1) ? 1 : -1;
	err = dx - dy;

	while (1)
	{
		OLED_DrawPoint((uint8_t)x, (uint8_t)y, dot);
		if (x == (int16_t)x1 && y == (int16_t)y1) break;
		e2 = err * 2;
		if (e2 > -dy) { err -= dy; x += sx; }
		if (e2 <  dx) { err += dx; y += sy; }
	}
}

/* 中点画圆，只画圆周 */
void OLED_DrawCircle(uint8_t xc, uint8_t yc, uint8_t r, uint8_t dot)
{
	int16_t x = 0, y = r, d = 3 - 2 * (int16_t)r;

	while (x <= y)
	{
		OLED_DrawPoint((uint8_t)(xc + x), (uint8_t)(yc + y), dot);
		OLED_DrawPoint((uint8_t)(xc - x), (uint8_t)(yc + y), dot);
		OLED_DrawPoint((uint8_t)(xc + x), (uint8_t)(yc - y), dot);
		OLED_DrawPoint((uint8_t)(xc - x), (uint8_t)(yc - y), dot);
		OLED_DrawPoint((uint8_t)(xc + y), (uint8_t)(yc + x), dot);
		OLED_DrawPoint((uint8_t)(xc - y), (uint8_t)(yc + x), dot);
		OLED_DrawPoint((uint8_t)(xc + y), (uint8_t)(yc - x), dot);
		OLED_DrawPoint((uint8_t)(xc - y), (uint8_t)(yc - x), dot);

		if (d < 0)
		{
			d += 4 * x + 6;
		}
		else
		{
			d += 4 * (x - y) + 10;
			y--;
		}
		x++;
	}
}

void OLED_DrawDisc(uint8_t xc, uint8_t yc, uint8_t r, uint8_t dot)
{
	int16_t dx, dy;

	for (dy = -(int16_t)r; dy <= (int16_t)r; dy++)
	{
		for (dx = -(int16_t)r; dx <= (int16_t)r; dx++)
		{
			if (dx * dx + dy * dy <= (int16_t)r * (int16_t)r + (int16_t)(r / 2))
			{
				OLED_DrawPoint((uint8_t)(xc + dx), (uint8_t)(yc + dy), dot);
			}
		}
	}
}

/* 8x16 字符：字库里每个字符 16 字节，前 8 个是上半，后 8 个是下半 */
void OLED_ShowChar(uint8_t x, uint8_t y, char ch)
{
	uint8_t i, j, t;

	if (ch < ' ' || ch > '~') return;   /* 只支持 ASCII 可见字符 */
	ch = (char)(ch - ' ');

	for (i = 0; i < 8; i++)
	{
		t = OLED_F8x16[(uint8_t)ch][i];
		for (j = 0; j < 8; j++)
		{
			OLED_DrawPoint((uint8_t)(x + i), (uint8_t)(y + j), (uint8_t)((t >> j) & 0x01));
		}

		t = OLED_F8x16[(uint8_t)ch][i + 8];
		for (j = 0; j < 8; j++)
		{
			OLED_DrawPoint((uint8_t)(x + i), (uint8_t)(y + 8 + j), (uint8_t)((t >> j) & 0x01));
		}
	}
}

void OLED_ShowString(uint8_t x, uint8_t y, const char *str)
{
	while (*str != '\0')
	{
		if (y > 48) break;                                          /* 底下放不下了 */
		if (x > 120) { x = 0; y = (uint8_t)(y + 16); continue; }    /* 换行 */

		OLED_ShowChar(x, y, *str);
		x = (uint8_t)(x + 8);
		str++;
	}
}

/* 定宽无符号整数：digits 是总位数，例如 123 配 digits=5 显示成 "00123"。
   定宽的好处是数字变化时后面的内容不会左右跳。 */
void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t digits)
{
	char buf[12];
	uint8_t i;

	if (digits == 0 || digits > 10) return;

	buf[digits] = '\0';
	for (i = 0; i < digits; i++)
	{
		buf[digits - 1 - i] = (char)('0' + (num % 10));
		num /= 10;
	}

	OLED_ShowString(x, y, buf);
}

/* 定宽带符号整数：前面多画一个 '+' 或 '-'，所以实际占 digits+1 个字符位。
   例如 -456 配 digits=5 显示成 "-00456"。 */
void OLED_ShowSignedNum(uint8_t x, uint8_t y, int32_t num, uint8_t digits)
{
	uint32_t v;
	char sign;

	if (digits == 0 || digits > 10) return;

	if (num < 0)
	{
		sign = '-';
		v = (uint32_t)(-num);
	}
	else
	{
		sign = '+';
		v = (uint32_t)num;
	}

	OLED_ShowChar(x, y, sign);
	OLED_ShowNum((uint8_t)(x + 8), y, v, digits);
}

/* 一位小数的定点数：val_x10 传"实际值乘 10"。
   例如 val_x10 = 23 显示成 "+2.3"（int_digits = 1），
        val_x10 = -114 显示成 "-11.4"（int_digits = 2）。
   角度用它显示，这样全程整数运算，不会把浮点库拉进工程。 */
void OLED_ShowFixed1(uint8_t x, uint8_t y, int32_t val_x10, uint8_t int_digits)
{
	uint32_t a;

	if (int_digits == 0 || int_digits > 6) return;

	a = (val_x10 < 0) ? (uint32_t)(-val_x10) : (uint32_t)val_x10;

	OLED_ShowChar(x, y, (val_x10 < 0) ? '-' : '+');
	OLED_ShowNum((uint8_t)(x + 8), y, a / 10, int_digits);
	OLED_ShowChar((uint8_t)(x + 8 + int_digits * 8), y, '.');
	OLED_ShowChar((uint8_t)(x + 16 + int_digits * 8), y, (char)('0' + (a % 10)));
}
