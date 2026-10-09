#ifndef __DELAY_H
#define __DELAY_H

#include "stm32f10x.h"

void delay_init(void);
void delay_us(uint32_t us);
void delay_ms(uint32_t ms);
uint32_t delay_millis(void);
uint16_t delay_micros16(void); /* 1MHz TIM2 wraps every 65.536ms */
void delay_tick(void); /* SysTick_Handler ×¨ÓÃ */

#endif
