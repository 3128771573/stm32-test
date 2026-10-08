#ifndef __BUZZER_H
#define __BUZZER_H

#include "stm32f10x.h"

/*---------------------------------------------------------------------------
  有源蜂鸣器（3 针：VCC / I-O / GND），低电平触发。
      VCC -> 3.3V
      GND -> GND
      I-O -> PB8

  注意：低电平触发的模块，在 MCU 把 PB8 拉高之前引脚是悬空的，
  上电瞬间可能"嘀"一声。这是正常现象，不是坏了。
  所以 main() 里第一件事就是 Buzzer_Init()，把这个窗口压到最短。
  如果它上电后一直响不停，说明这个模块悬空时就是低电平，
  在 I-O 和 3.3V 之间加一个 10k 上拉电阻即可。
---------------------------------------------------------------------------*/

void Buzzer_Init(void);
void Buzzer_On(void);               /* 响 */
void Buzzer_Off(void);              /* 不响 */
void Buzzer_Beep(uint16_t ms);      /* 短鸣一声 */

#endif
