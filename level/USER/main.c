#include "stm32f10x.h"
#include "delay.h"
#include "led.h"
#include "i2c.h"
#include "oled.h"
#include "mpu6050.h"
#include "buzzer.h"

/*---------------------------------------------------------------------------
  第 7 步：气泡水平仪 + 蜂鸣器提示（整机功能齐了）

  【画面布局】
    左半屏（x 0~62）画水平仪：
      外环 r=29、内参考环 r=12、上下左右四个刻度、靶心
      气泡是个实心圆 r=4，跟着倾斜方向跑
      气泡落在靶心上时，靶心会填实 —— 这是"已经水平"的视觉提示
    右半屏（x 66 起）显示两个角度数字：
      PITCH / ROLL，单位度，一位小数

  【量程】
    15.0 度对应跑到环内壁（约 24 像素），再大就贴在壁上不动了。
    水平仪真正要用的范围是正负几度，15 度做满量程手感刚好。

  【刷新率与滤波】
    每 5ms 左右采一个点（传感器本身 200Hz 出数），一轮采 8 个点刷一次屏。
    一轮 = 8 x 5.6ms 采样 + 37ms 整屏刷新 = 约 82ms，也就是【约 12Hz】。
    有效采样率约 98Hz，一阶低通 alpha = 1/32，
    折算成截止频率约 0.5Hz —— 和上一版（4.6Hz 刷新）的平滑度一样，
    但刷新快了将近两倍半。

    想再快就把 SAMPLE_PER_DISPLAY 调小；想让画面更跟手就把 LPF_SHIFT 调小。

  【方向约定】
    PITCH_DIR / ROLL_DIR 决定数字的正负。
    BUBBLE_X_DIR / BUBBLE_Y_DIR 决定气泡往哪边跑。
    上机发现跑反了，把对应的宏改成相反数即可，只改一个字符。

  【蜂鸣器】
    蜂鸣器接在 PB8，有源、低电平触发。
    只在"刚摆平"的那一刻短鸣一声，不会一直叫。
    进阈值 1.0 度、出阈值 2.0 度，中间留 1 度的滞回——
    否则你把板子停在临界角度上，它会来回叫个不停。
    另外还要连续两轮都在阈值内才认，避免被一两个跳点误触发。
    上电时会先短鸣一声，那是用来验证蜂鸣器接好了没有。
---------------------------------------------------------------------------*/

#define CAL_SAMPLES         100     /* 校准采样点数，约 1 秒 */
#define SAMPLE_PER_DISPLAY  8       /* 每次刷屏前采多少个点 */
#define SAMPLE_INTERVAL_MS  5       /* 采样间隔，对应传感器 200Hz 出数 */
#define LPF_SHIFT           5       /* 一阶低通 alpha = 1/32，截止约 0.5Hz */
#define DEG_X10_PER_RAD     573     /* 57.2958 度/弧度 * 10 */
#define MIN_CAL_Z           8000    /* 校准时 Z 至少要这么大，否则说明板子没放平 */
#define HEARTBEAT_LOOPS     4       /* 每刷几次屏闪一下灯，约 1 秒 */

/* 水平判定与蜂鸣器 */
#define LEVEL_ENTER_X10     10      /* 进入水平判定：1.0 度 */
#define LEVEL_EXIT_X10      20      /* 退出水平判定：2.0 度（滞回） */
#define BEEP_HOLD_CYCLES    2       /* 连续这么多轮在阈值内才鸣叫 */
#define BEEP_EXIT_CYCLES    2       /* 连续这么多轮出阈值才重新武装 */
#define BEEP_MS             120     /* 鸣叫时长 */
#define BOOT_BEEP_MS        60      /* 开机自检短鸣 */

#define PITCH_DIR           (-1)
#define ROLL_DIR            (1)
#define BUBBLE_X_DIR        (1)     /* 气泡水平方向 */
#define BUBBLE_Y_DIR        (1)     /* 气泡垂直方向：1 表示角度为正时气泡往上跑 */

/* ---- 水平仪几何 ---- */
#define HUD_CX          31
#define HUD_CY          32
#define HUD_RING_R      29
#define HUD_INNER_R     12
#define HUD_BUBBLE_R    4
#define HUD_MAX_OFFSET  (HUD_RING_R - HUD_BUBBLE_R - 1)     /* 气泡中心离靶心最远多少 */
#define HUD_FS_X10      150                                 /* 满量程 15.0 度 */

#define HUD_TEXT_X      66

/* 滤波器状态：放大 256 倍存放，避免整数除法逐次丢精度 */
static int32_t lpf[3];
static int32_t base_ax = 0, base_ay = 0, base_az = 0;

/* 蜂鸣器状态机 */
static uint8_t beep_armed   = 1;    /* 是否允许下一次鸣叫 */
static uint8_t inside_cnt   = 0;    /* 连续在进阈值内的轮数 */
static uint8_t outside_cnt  = 0;    /* 连续在出阈值外的轮数 */

static int32_t iabs32(int32_t v)
{
	return (v < 0) ? -v : v;
}

/* 整数开平方，用来把气泡限制在环内（径向限幅） */
static uint32_t isqrt32(uint32_t n)
{
	uint32_t res = 0;
	uint32_t bit = 1UL << 30;

	while (bit > n) bit >>= 2;

	while (bit)
	{
		if (n >= res + bit)
		{
			n -= res + bit;
			res = (res >> 1) + bit;
		}
		else
		{
			res >>= 1;
		}
		bit >>= 2;
	}
	return res;
}

static void mpu_filter_reset(void)
{
	uint8_t k;
	for (k = 0; k < 3; k++) lpf[k] = 0;
}

static void mpu_filter_feed(const int16_t *acc, uint8_t first)
{
	uint8_t k;

	if (first)
	{
		for (k = 0; k < 3; k++) lpf[k] = ((int32_t)acc[k]) << 8;
		return;
	}

	for (k = 0; k < 3; k++)
	{
		lpf[k] += ((((int32_t)acc[k]) << 8) - lpf[k]) >> LPF_SHIFT;
	}
}

static int16_t mpu_filter_get(uint8_t k)
{
	return (int16_t)(lpf[k] >> 8);
}

static void blink(uint8_t times, uint16_t on_ms, uint16_t off_ms)
{
	while (times--)
	{
		LED_On();
		delay_ms(on_ms);
		LED_Off();
		delay_ms(off_ms);
	}
}

/*---------------------------------------------------------------------------
  开机零点校准：采一批点取平均当基准。
  角度全是相对这个基准算的，所以零偏和标度误差都能抵消。
---------------------------------------------------------------------------*/
static uint8_t mpu_calibrate(void)
{
	uint8_t  i;
	uint8_t  n = 0;
	int16_t  ax, ay, az;
	int32_t  sx = 0, sy = 0, sz = 0;

	OLED_Clear();
	OLED_ShowString(20, 8,  "CALIBRATING");
	OLED_ShowString(12, 32, "KEEP IT LEVEL");
	OLED_Refresh();

	for (i = 0; i < CAL_SAMPLES; i++)
	{
		if (MPU6050_ReadAccel(&ax, &ay, &az) == 0)
		{
			sx += ax;
			sy += ay;
			sz += az;
			n++;
		}
		delay_ms(10);
	}

	if (n == 0) return 1;

	base_ax = sx / n;
	base_ay = sy / n;
	base_az = sz / n;

	/* 校准时板子如果立着放，az 会很小，除下去角度就不对了，这里兜个底 */
	if (base_az < MIN_CAL_Z) base_az = MIN_CAL_Z;

	return 0;
}

/* 画整个水平仪界面 */
static void draw_level(int32_t pitch_x10, int32_t roll_x10, uint8_t level)
{
	int32_t dx, dy, d2, d;

	/* 外环与内参考环 */
	OLED_DrawCircle(HUD_CX, HUD_CY, HUD_RING_R, 1);
	OLED_DrawCircle(HUD_CX, HUD_CY, HUD_INNER_R, 1);

	/* 上下左右四个刻度，贴在环内侧 */
	OLED_DrawLine(HUD_CX, HUD_CY - HUD_RING_R + 1, HUD_CX, HUD_CY - HUD_RING_R + 6, 1);
	OLED_DrawLine(HUD_CX, HUD_CY + HUD_RING_R - 6, HUD_CX, HUD_CY + HUD_RING_R - 1, 1);
	OLED_DrawLine(HUD_CX - HUD_RING_R + 1, HUD_CY, HUD_CX - HUD_RING_R + 6, HUD_CY, 1);
	OLED_DrawLine(HUD_CX + HUD_RING_R - 6, HUD_CY, HUD_CX + HUD_RING_R - 1, HUD_CY, 1);

	/* 靶心：水平时填实，这是最直观的"已水平"提示 */
	if (level) OLED_DrawDisc(HUD_CX, HUD_CY, 3, 1);
	else       OLED_DrawCircle(HUD_CX, HUD_CY, 3, 1);

	/* 角度换算成气泡偏移 */
	dx = (roll_x10  * HUD_MAX_OFFSET) / HUD_FS_X10 * BUBBLE_X_DIR;
	dy = (pitch_x10 * HUD_MAX_OFFSET) / HUD_FS_X10 * BUBBLE_Y_DIR;

	/* 径向限幅：超了就沿原方向压回环内，别让气泡跑出圆环 */
	d2 = dx * dx + dy * dy;
	if (d2 > (int32_t)HUD_MAX_OFFSET * HUD_MAX_OFFSET)
	{
		d = (int32_t)isqrt32((uint32_t)d2);
		if (d > 0)
		{
			dx = dx * HUD_MAX_OFFSET / d;
			dy = dy * HUD_MAX_OFFSET / d;
		}
	}

	/* 屏幕 y 轴向下，所以正角度要让气泡往上走就得用减法 */
	OLED_DrawDisc((uint8_t)(HUD_CX + dx), (uint8_t)(HUD_CY - dy), HUD_BUBBLE_R, 1);

	/* 右半屏的数字 */
	OLED_ShowString(HUD_TEXT_X, 0,  "PITCH");
	OLED_ShowFixed1(HUD_TEXT_X, 16, pitch_x10, 2);
	OLED_ShowString(HUD_TEXT_X, 32, "ROLL");
	OLED_ShowFixed1(HUD_TEXT_X, 48, roll_x10, 2);
}

int main(void)
{
	uint8_t  oled_ok, mpu_ok, mpu_id, mpu_ready;
	uint8_t  i, hb_cnt = 0, got = 0, first = 1;
	int16_t  acc[3], gyro[3], temp_raw = 0;
	int32_t  dx, dy, pitch_x10, roll_x10;

	/* 蜂鸣器放在最前面初始化：低电平触发的模块在引脚悬空时可能乱响，
	   所以第一件事就是把它拉到"不响"的状态 */
	Buzzer_Init();
	LED_Init();
	LED_On();

	delay_init();
	Buzzer_Beep(BOOT_BEEP_MS);      /* 开机短鸣，验证蜂鸣器接好了 */
	SoftI2C_Init();
	SoftI2C_BusRecover();

	delay_ms(300);

	oled_ok = SoftI2C_Probe(OLED_ADDR7);
	mpu_ok  = SoftI2C_Probe(MPU6050_ADDR7);

	if (!oled_ok)
	{
		LED_Off();
		while (1)
		{
			blink(mpu_ok ? 3 : 5, 80, 120);
			delay_ms(1200);
		}
	}

	OLED_Init();

	mpu_id = mpu_ok ? MPU6050_ReadID() : 0x00;
	mpu_ready = (mpu_ok && mpu_id == MPU6050_ADDR7 && MPU6050_Init() == 0) ? 1 : 0;

	if (mpu_ready)
	{
		if (mpu_calibrate() != 0) mpu_ready = 0;
	}

	LED_Off();

	while (1)
	{
		uint8_t  level;

		got = 0;
		mpu_filter_reset();
		first = 1;

		if (mpu_ready)
		{
			for (i = 0; i < SAMPLE_PER_DISPLAY; i++)
			{
				if (MPU6050_ReadAll(acc, gyro, &temp_raw) == 0)
				{
					mpu_filter_feed(acc, first);
					first = 0;
					got = 1;
				}
				delay_ms(SAMPLE_INTERVAL_MS);
			}
		}

		OLED_Clear();

		if (mpu_ready && got)
		{
			dx = (int32_t)mpu_filter_get(0) - base_ax;
			dy = (int32_t)mpu_filter_get(1) - base_ay;

			pitch_x10 = (int32_t)(PITCH_DIR * ((dx * DEG_X10_PER_RAD) / base_az));
			roll_x10  = (int32_t)(ROLL_DIR  * ((dy * DEG_X10_PER_RAD) / base_az));

			level = (iabs32(pitch_x10) <= LEVEL_ENTER_X10 &&
			         iabs32(roll_x10)  <= LEVEL_ENTER_X10) ? 1 : 0;

			draw_level(pitch_x10, roll_x10, level);

			/* ---- 蜂鸣器：带滞回的单次提示 ---- */
			if (level)
			{
				if (inside_cnt < 255) inside_cnt++;
			}
			else
			{
				inside_cnt = 0;
			}

			if (iabs32(pitch_x10) >= LEVEL_EXIT_X10 ||
			    iabs32(roll_x10)  >= LEVEL_EXIT_X10)
			{
				if (outside_cnt < 255) outside_cnt++;
			}
			else
			{
				outside_cnt = 0;
			}

			if (beep_armed && inside_cnt >= BEEP_HOLD_CYCLES)
			{
				Buzzer_Beep(BEEP_MS);       /* 刚摆平，短鸣一声 */
				beep_armed = 0;
				inside_cnt = 0;
			}
			else if (!beep_armed && outside_cnt >= BEEP_EXIT_CYCLES)
			{
				beep_armed = 1;             /* 离开水平区，重新武装 */
				outside_cnt = 0;
			}
		}
		else if (mpu_ready)
		{
			OLED_ShowString(0, 0, "READ ERR");
			OLED_ShowString(0, 24, "CHECK WIRING");
		}
		else
		{
			OLED_ShowString(0, 0, "MPU6050 ERR");
			OLED_ShowNum(96, 0, (uint32_t)mpu_id, 3);
			OLED_ShowString(0, 24, "ID SHOULD BE 104");
		}

		OLED_Refresh();

		if (++hb_cnt >= HEARTBEAT_LOOPS)
		{
			hb_cnt = 0;
			LED_On();
			delay_ms(40);
			LED_Off();
		}
	}
}
