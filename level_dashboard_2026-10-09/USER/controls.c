#include "controls.h"

static volatile int8_t turns;
static volatile uint8_t button_event, light_mode;
static uint8_t old_ab, key_last, key_ticks, long_sent;
static volatile uint8_t key_stable;
static int8_t quarter_steps;
static uint16_t hold_ticks, light_ticks, error_ticks, fade_ticks, save_hold;
static uint8_t active_mode, light_divider;
static uint16_t light_value[3], fade_from[3]; /* Perceived intensity, Q8. */
static volatile uint16_t level_distance;

#define LED_PWM_PERIOD 3600U
#define LED_PWM_HZ 2000U
#define LIGHT_FADE_MS 300U

static void light_pwm_init(void)
{
    RCC_ClocksTypeDef clocks;
    uint32_t timer_clock;
    RCC_GetClocksFreq(&clocks);
    timer_clock=clocks.PCLK2_Frequency;
    if(clocks.PCLK2_Frequency!=clocks.HCLK_Frequency)timer_clock*=2;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1,ENABLE);
    RCC_APB2PeriphResetCmd(RCC_APB2Periph_TIM1,ENABLE);
    RCC_APB2PeriphResetCmd(RCC_APB2Periph_TIM1,DISABLE);
    /* PA8/PA9/PA10 = TIM1 CH1/2/3; PWM1, active high, shadowed duties. */
    TIM1->PSC=timer_clock/(LED_PWM_HZ*LED_PWM_PERIOD)-1;
    TIM1->ARR=LED_PWM_PERIOD-1;
    TIM1->CCR1=TIM1->CCR2=TIM1->CCR3=0;
    TIM1->CCMR1=TIM_CCMR1_OC1PE|TIM_CCMR1_OC1M_1|TIM_CCMR1_OC1M_2|
        TIM_CCMR1_OC2PE|TIM_CCMR1_OC2M_1|TIM_CCMR1_OC2M_2;
    TIM1->CCMR2=TIM_CCMR2_OC3PE|TIM_CCMR2_OC3M_1|TIM_CCMR2_OC3M_2;
    TIM1->CCER=TIM_CCER_CC1E|TIM_CCER_CC2E|TIM_CCER_CC3E;
    TIM1->CR1=TIM_CR1_ARPE;
    TIM1->EGR=TIM_EGR_UG;
    TIM1->SR=0;
    TIM1->BDTR=TIM_BDTR_MOE; /* Advanced timer needs main output enable. */
    TIM1->CR1|=TIM_CR1_CEN;
}

/* Smoothstep (0..1024), zero slope at both ends, integer-only. */
static uint16_t ease(uint16_t value,uint16_t span)
{
    uint32_t q=(uint32_t)value*1024/span;
    return (uint16_t)(q*q*(3072-2*q)>>20);
}
static uint16_t breath(uint16_t tick,uint16_t period,uint8_t peak)
{
    uint16_t half=period/2,position=tick<half?tick:period-tick;
    return (uint16_t)(14U*256U+(uint32_t)(peak-14)*256U*ease(position,half)/1024U);
}
static uint16_t pwm_duty(uint16_t perceived)
{
    /* Gamma ~2: retain Q8 precision so low light doesn't jump in big steps. */
    uint32_t q=(uint32_t)perceived*1024U/(255U*256U);
    return (uint16_t)(q*q*LED_PWM_PERIOD/1048576UL);
}
static void light_tick(void)
{
    uint8_t mode=light_mode,k;
    uint16_t target[3]={0,0,0},weight;
    if(save_hold)save_hold--;
    if(active_mode==LIGHT_SAVE&&save_hold&&mode!=LIGHT_ERROR)mode=LIGHT_SAVE;
    if(mode!=active_mode){
        active_mode=mode;fade_ticks=0;
        for(k=0;k<3;k++)fade_from[k]=light_value[k];
        if(mode==LIGHT_SAVE)save_hold=500; /* Brief Flash writes remain visible. */
        if(mode==LIGHT_ERROR)error_ticks=0;
    }
    if(fade_ticks<LIGHT_FADE_MS)fade_ticks++;
    light_ticks=(uint16_t)((light_ticks+1)%3000);
    error_ticks=(uint16_t)((error_ticks+1)%1600);
    if(++light_divider<5)return; /* 200Hz envelope, PWM itself runs in hardware. */
    light_divider=0;
    if(mode==LIGHT_LEVEL)target[1]=breath(light_ticks,3000,140);
    else if(mode==LIGHT_ERROR)target[0]=breath(error_ticks,1600,170);
    else if(mode==LIGHT_SAVE){target[1]=100U*256U;target[2]=120U*256U;}
    else if(mode==LIGHT_OFFSET){
        uint16_t distance=level_distance;
        target[0]=(uint16_t)((uint32_t)distance*190U*256U/150U);
        target[1]=(uint16_t)((uint32_t)(150-distance)*140U*256U/150U);
    }
    else target[2]=breath(light_ticks,3000,180);
    weight=ease(fade_ticks,LIGHT_FADE_MS);
    for(k=0;k<3;k++){
        if(mode==LIGHT_OFFSET&&fade_ticks==LIGHT_FADE_MS){
            int32_t delta=(int32_t)target[k]-light_value[k];
            /* 60ms smoothing; rounded steps also converge at very low light. */
            light_value[k]=(uint16_t)(light_value[k]+(delta>=0?(delta+11)/12:(delta-11)/12));
        }else light_value[k]=(uint16_t)((int32_t)fade_from[k]+
            ((int32_t)target[k]-fade_from[k])*weight/1024);
    }
    TIM1->CCR1=pwm_duty(light_value[0]);
    TIM1->CCR2=pwm_duty(light_value[1]);
    TIM1->CCR3=pwm_duty(light_value[2]);
}

void Controls_Init(void)
{
    GPIO_InitTypeDef gpio;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);
    gpio.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_10;
    gpio.GPIO_Mode = GPIO_Mode_IPU; gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOB, &gpio);
    turns=button_event=light_mode=key_ticks=long_sent=0;
    quarter_steps=0;hold_ticks=light_ticks=error_ticks=fade_ticks=save_hold=0;
    active_mode=LIGHT_IDLE;light_divider=0;
    level_distance=0;
    light_value[0]=light_value[1]=light_value[2]=0;
    fade_from[0]=fade_from[1]=fade_from[2]=0;
    GPIO_ResetBits(GPIOA, GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10);
    gpio.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP; GPIO_Init(GPIOA, &gpio);
    light_pwm_init();
    old_ab = (uint8_t)(GPIOB->IDR & 3);
    key_stable = key_last = (GPIOB->IDR & GPIO_Pin_10) != 0;
}

void Controls_Tick(void)
{
    static const int8_t gray[16] = {0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
    uint8_t ab = (uint8_t)(GPIOB->IDR & 3), key;
    if (ab != old_ab) {
        if ((ab ^ old_ab) == 3) quarter_steps = 0; /* missed/illegal edge */
        else quarter_steps += gray[(old_ab << 2) | ab];
        old_ab = ab;
        if (quarter_steps >= ENCODER_EDGES_PER_STEP) {
            if (turns < 20) turns++;
            quarter_steps = 0;
        } else if (quarter_steps <= -ENCODER_EDGES_PER_STEP) {
            if (turns > -20) turns--;
            quarter_steps = 0;
        }
    }
    key = (GPIOB->IDR & GPIO_Pin_10) != 0;
    if (key != key_last) { key_last = key; key_ticks = 0; }
    else if (key_ticks < 20) key_ticks++;
    if (key_ticks == 20 && key != key_stable) {
        key_stable = key;
        if (!key) { hold_ticks = 0; long_sent = 0; }
        else if (!long_sent) button_event |= CONTROL_CLICK;
    }
    if (!key_stable && !long_sent && ++hold_ticks >= 800) {
        button_event |= CONTROL_HOME; long_sent = 1;
    }
    light_tick();
}

int8_t Controls_TakeTurn(void)
{
    uint32_t mask = __get_PRIMASK(); int8_t result;
    __disable_irq(); result = turns; turns = 0; __set_PRIMASK(mask); return result;
}
uint8_t Controls_TakeButton(void)
{
    uint32_t mask = __get_PRIMASK(); uint8_t result;
    __disable_irq(); result = button_event; button_event = 0; __set_PRIMASK(mask); return result;
}
void Controls_SetLight(uint8_t mode) { light_mode = mode; }
uint8_t Controls_ButtonDown(void) { return !key_stable; }
void Controls_SetLevelOffset(int32_t pitch,int32_t roll)
{
    uint32_t n,result=0,bit=1UL<<30;
    if(pitch>150)pitch=150;else if(pitch< -150)pitch=-150;
    if(roll>150)roll=150;else if(roll< -150)roll=-150;
    n=(uint32_t)(pitch*pitch+roll*roll);
    while(bit>n)bit>>=2;
    while(bit){if(n>=result+bit){n-=result+bit;result=(result>>1)+bit;}else result>>=1;bit>>=2;}
    level_distance=(uint16_t)(result>150?150:result);
}
uint16_t Controls_LevelDistance(void) { return level_distance; }
