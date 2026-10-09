#ifndef CONTROLS_H
#define CONTROLS_H
#include "stm32f10x.h"
/* PB0/PB1: encoder A/B, PB10: push (all active low). PA8/9/10: R/G/B. */
#define ENCODER_EDGES_PER_STEP 4
enum { CONTROL_CLICK = 1, CONTROL_HOME = 2 };
enum { LIGHT_IDLE, LIGHT_LEVEL, LIGHT_ERROR, LIGHT_SAVE, LIGHT_OFFSET };
void Controls_Init(void);
void Controls_Tick(void); /* 1ms keys, 5ms fade; TIM1 runs LED PWM autonomously. */
int8_t Controls_TakeTurn(void);
uint8_t Controls_TakeButton(void);
void Controls_SetLight(uint8_t mode);
uint8_t Controls_ButtonDown(void); /* Debounced PB10, independent of Flash. */
void Controls_SetLevelOffset(int32_t pitch_x10,int32_t roll_x10);
uint16_t Controls_LevelDistance(void); /* Radial degrees x10, capped at 15deg. */
#endif
