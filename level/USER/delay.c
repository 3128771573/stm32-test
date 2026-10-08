#include "delay.h"

/*---------------------------------------------------------------------------
  用 SysTick 普通计数模式做延时，不占用任何定时器中断。
  SysTick 时钟取 HCLK/8，本板 72MHz 时是 9MHz，所以 1us 数 9 个。
  注意：SysTick 的 LOAD 是 24 位，单次最多 0xFFFFFF 个计数，
        对应 72MHz 下约 1864ms，本文件已做上限保护，不会溢出。
---------------------------------------------------------------------------*/

static uint8_t fac_us = 0;      /* 每个 us 需要几个 SysTick 计数 */

void delay_init(void)
{
	SysTick_CLKSourceConfig(SysTick_CLKSource_HCLK_Div8);
	fac_us = (uint8_t)(SystemCoreClock / 8000000);      /* 72MHz -> 9 */
}

void delay_us(uint32_t us)
{
	uint32_t load;

	/* us 传 0 时 LOAD 也是 0，计数器不会置 COUNTFLAG，会死等，所以直接返回 */
	if (us == 0 || fac_us == 0) return;

	load = us * fac_us;
	if (load > 0xFFFFFFUL) load = 0xFFFFFFUL;           /* 24 位上限保护 */

	SysTick->LOAD = load;
	SysTick->VAL  = 0;
	SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;

	while ((SysTick->CTRL & SysTick_CTRL_ENABLE_Msk) &&
	       !(SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk))
	{
		/* 等计数到 0 */
	}

	SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
	SysTick->VAL   = 0;
}

/* 毫秒延时用循环实现，这样传多大的数都不会溢出 */
void delay_ms(uint32_t ms)
{
	while (ms--)
	{
		delay_us(1000);
	}
}
