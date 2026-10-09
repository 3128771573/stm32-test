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
static uint8_t contrast_now=0xCF,contrast_target=0xCF;
static uint32_t contrast_next;
static uint8_t contrast_wait;
static uint8_t sent_gram[8][128];
static uint8_t refresh_busy, refresh_block, sent_valid;
static uint8_t recovery_active,recovery_step,recovery_wait;
static uint32_t recovery_next;

/* Complete commands are kept in separate short transactions. Runtime recovery
   sends at most one row per service call, with ACK checked by WriteBytes. */
static const uint8_t init_commands[][3]={
    {0x00,0xAE,0}, {0x00,0xD5,0x80}, {0x00,0xA8,0x3F},
    {0x00,0xD3,0x00}, {0x00,0x40,0}, {0x00,0x20,0x00},
    {0x00,0xA1,0}, {0x00,0xC8,0}, {0x00,0xDA,0x12},
    {0x00,0x81,0xCF}, {0x00,0xD9,0xF1}, {0x00,0xDB,0x30},
    {0x00,0xA4,0}, {0x00,0xA6,0}, {0x00,0x8D,0x14},
    {0x00,0xAF,0}
};
static const uint8_t init_sizes[]={2,3,3,3,2,3,2,2,3,3,3,3,2,2,3,2};
#define INIT_COMMAND_COUNT (sizeof(init_sizes)/sizeof(init_sizes[0]))

static void need_recovery(void)
{
    refresh_busy=sent_valid=0;
    recovery_active=1;recovery_step=recovery_wait=0;
    SoftI2C_BusRecover();
}

/* 本工程自绘 5x7 大写字母，紧凑标题和状态条。每列低位在上。 */
static const uint8_t small_font[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}
};

static uint8_t small_bits(char ch, uint8_t col)
{
    static const uint8_t digits[10][5] = {
        {0x3E,0x41,0x41,0x41,0x3E},{0,0x42,0x7F,0x40,0},
        {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
        {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
        {0x3C,0x4A,0x49,0x49,0x30},{1,0x71,9,5,3},
        {0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1E}
    };
    if(col>=5) return 0;
    if(ch>='A' && ch<='Z') return small_font[ch-'A'][col];
    if(ch>='0' && ch<='9') return digits[ch-'0'][col];
    if(ch=='-') return 0x08;
    if(ch=='+') return col==2 ? 0x3E : 0x08;
    if(ch=='.') return col==2 ? 0x60 : 0;
    if(ch==':') return col==2 ? 0x36 : 0;
    if(ch=='/') return (uint8_t)(0x40>>(col*1));
    if(ch=='%') {static const uint8_t b[5]={0x63,0x13,8,0x64,0x63};return b[col];}
    if(ch=='#') {static const uint8_t b[5]={0x14,0x7F,0x14,0x7F,0x14};return b[col];}
    return 0;
}
static void small_string(uint8_t x, uint8_t y, const char *str, uint8_t inverse)
{
    uint8_t col, row, bits;
    while (*str && x <= 122 && y <= 57) {
        for (col = 0; col < 6; col++) {
            bits = small_bits(*str,col);
            for (row = 0; row < 7; row++)
                OLED_DrawPoint(x + col, y + row, ((bits >> row) & 1) ^ inverse);
        }
        x += 6; str++;
    }
}
void OLED_ShowSmallString(uint8_t x, uint8_t y, const char *str) { small_string(x, y, str, 0); }
void OLED_ShowSmallStringInverse(uint8_t x, uint8_t y, const char *str) { small_string(x, y, str, 1); }
void OLED_ShowLargeString(uint8_t x,uint8_t y,const char *str)
{
    uint8_t col,row,dx,dy,bits;
    while(*str && x<=116 && y<=50) {
        for(col=0;col<6;col++) {
            bits=small_bits(*str,col);
            for(row=0;row<7;row++) for(dx=0;dx<2;dx++) for(dy=0;dy<2;dy++)
                OLED_DrawPoint(x+col*2+dx,y+row*2+dy,(bits>>row)&1);
        }
        x+=12;str++;
    }
}

void OLED_Init(void)
{
    uint8_t k;
    contrast_now=contrast_target=0xCF;contrast_next=0;contrast_wait=0;
    refresh_busy=refresh_block=sent_valid=0;
    recovery_active=recovery_step=recovery_wait=0;
    for(k=0;k<INIT_COMMAND_COUNT;k++) {
        if(SoftI2C_WriteBytes(OLED_ADDR7,init_commands[k],init_sizes[k])) {
            need_recovery();break;
        }
    }
    OLED_Clear();OLED_Refresh();
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

/* 差分短块刷新：已发送缓存仅用于比较，不是 OLED 的硬件双缓冲。
   每次发一个 32 字节块，主循环可在块间读取 MPU6050。
   刷新期间不要改变 OLED_GRAM；完成后才允许绘制下一帧。 */
uint8_t OLED_RefreshBusy(void) { return refresh_busy; }
void OLED_SetIdle(uint8_t idle)
{
    uint8_t target=idle?0x28:0xCF;
    if(target!=contrast_target) {
        contrast_target=target;contrast_wait=0;
    }
}
void OLED_Service(uint32_t now)
{
    uint8_t next,cmd[3]={0x00,0x81,0};
    if(recovery_active) {
        if(!recovery_wait) {
            recovery_next=now+500;recovery_wait=1;return;
        }
        if((int32_t)(now-recovery_next)<0)return;
        if(SoftI2C_WriteBytes(OLED_ADDR7,init_commands[recovery_step],init_sizes[recovery_step])) {
            recovery_step=0;recovery_next=now+500;SoftI2C_BusRecover();return;
        }
        recovery_next=now;
        if(++recovery_step==INIT_COMMAND_COUNT) {
            recovery_active=recovery_wait=0;
            contrast_now=0xCF;contrast_wait=0;
            sent_valid=0;OLED_StartRefresh();
        }
        return;
    }
    if(refresh_busy||contrast_now==contrast_target||
        (contrast_wait&&(int32_t)(now-contrast_next)<0))return;
    /* Wake in one transaction; dim gently without filling or blanking the screen. */
    next=contrast_target>contrast_now?contrast_target:
        (uint8_t)(contrast_now-contrast_target>8?contrast_now-8:contrast_target);
    cmd[2]=next;
    if(SoftI2C_WriteBytes(OLED_ADDR7,cmd,3)){
        need_recovery();recovery_next=now+500;recovery_wait=1;return;
    }
    contrast_now=next;contrast_next=now+40;contrast_wait=1;
}
void OLED_StartRefresh(void)
{
    if (refresh_busy||recovery_active) return;
    refresh_block = 0;
    refresh_busy = 1;
}
void OLED_RefreshStep(void)
{
    uint8_t page, col, i, changed;
    uint8_t cmd[7] = {0x00, 0x21, 0, 0, 0x22, 0, 0};
    uint8_t data[33];
    if (!refresh_busy) return;
    while (refresh_block < 32) {
        page = refresh_block / 4;
        col = (refresh_block % 4) * 32;
        changed = !sent_valid;
        for (i = 0; i < 32; i++)
            if (sent_gram[page][col + i] != OLED_GRAM[page][col + i]) changed = 1;
        refresh_block++;
        if (!changed) continue;
        cmd[2] = col; cmd[3] = col + 31; cmd[5] = cmd[6] = page;
        data[0] = 0x40;
        for (i = 0; i < 32; i++) data[i + 1] = OLED_GRAM[page][col + i];
        if (SoftI2C_WriteBytes(OLED_ADDR7, cmd, 7) ||
            SoftI2C_WriteBytes(OLED_ADDR7, data, 33)) {
            /* Device configuration may also have reset after an OLED power loss. */
            need_recovery();
            return;
        }
        for (i = 0; i < 32; i++) sent_gram[page][col + i] = data[i + 1];
        break;
    }
    if (refresh_block == 32) { refresh_busy = 0; sent_valid = 1; }
}
void OLED_Refresh(void)
{
    OLED_StartRefresh();
    while (OLED_RefreshBusy()) OLED_RefreshStep();
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
