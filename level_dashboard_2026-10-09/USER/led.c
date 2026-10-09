#include "led.h"

/*---------------------------------------------------------------------------
  板载 LED 在 PC13，低电平点亮。
  它同时承担两个任务：
    1. 正常心跳（说明程序活着）
    2. 报码：总线扫描的结果用闪灯次数报出来，见 main.c
---------------------------------------------------------------------------*/

void LED_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);

	GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_13;
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOC, &GPIO_InitStructure);

	LED_Off();
}

void LED_On(void)
{
	GPIO_ResetBits(GPIOC, GPIO_Pin_13);     /* 低电平点亮 */
}

void LED_Off(void)
{
	GPIO_SetBits(GPIOC, GPIO_Pin_13);
}
