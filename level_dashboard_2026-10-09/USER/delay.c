#include "delay.h"
static volatile uint32_t tick_ms;
void delay_init(void)
{
    RCC_ClocksTypeDef clocks;
    uint32_t timer_clock;
    RCC_GetClocksFreq(&clocks);
    timer_clock = clocks.PCLK1_Frequency;
    if (clocks.PCLK1_Frequency != clocks.HCLK_Frequency) timer_clock *= 2;
    /* TIM2 在 1MHz 自由运行；SysTick 每 1ms 仅计时。 */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
    TIM2->CR1 = 0; TIM2->PSC = timer_clock / 1000000UL - 1;
    TIM2->ARR = 0xFFFF; TIM2->EGR = 1; TIM2->SR = 0; TIM2->CNT = 0; TIM2->CR1 = 1;
    tick_ms = 0;
    SysTick_Config(SystemCoreClock / 1000UL);
}
void delay_tick(void) { tick_ms++; }
uint32_t delay_millis(void) { return tick_ms; }
uint16_t delay_micros16(void) { return (uint16_t)TIM2->CNT; }
void delay_us(uint32_t us)
{
    while (us) {
        uint16_t span = (uint16_t)(us > 60000UL ? 60000UL : us);
        uint16_t start = (uint16_t)TIM2->CNT;
        while ((uint16_t)((uint16_t)TIM2->CNT - start) < span) { }
        us -= span;
    }
}
void delay_ms(uint32_t ms)
{
    uint32_t start = delay_millis();
    while ((uint32_t)(delay_millis() - start) < ms) { }
}
