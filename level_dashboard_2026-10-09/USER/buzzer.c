#include "buzzer.h"
#include "delay.h"

#define BUZZER_PORT     GPIOB
#define BUZZER_PIN      GPIO_Pin_8

void Buzzer_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

	GPIO_InitStructure.GPIO_Pin   = BUZZER_PIN;
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	/* 先预置高电平，再切换输出，避免低电平毛刺。 */
	GPIO_SetBits(BUZZER_PORT, BUZZER_PIN);
	GPIO_Init(BUZZER_PORT, &GPIO_InitStructure);

	Buzzer_Off();                   /* 先拉高：不响 */
}

void Buzzer_On(void)
{
	GPIO_ResetBits(BUZZER_PORT, BUZZER_PIN);    /* 低电平触发 */
}

void Buzzer_Off(void)
{
	GPIO_SetBits(BUZZER_PORT, BUZZER_PIN);
}

void Buzzer_Beep(uint16_t ms)
{
	if (ms == 0) return;
	Buzzer_On();
	delay_ms(ms);
	Buzzer_Off();
}
