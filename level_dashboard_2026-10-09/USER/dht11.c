#include "dht11.h"
#include "delay.h"
#include <string.h>

/* PA1/EXTI1 falling edges, TIM2 1MHz timestamps. No global IRQ masking. */
static DHT11_Data data;
static volatile uint8_t rx[5], edge_count, rx_done, receiving;
static volatile uint16_t edge_time;
static uint8_t state;
static uint32_t next_read, deadline;

static void data_mode(GPIOMode_TypeDef mode)
{
    GPIO_InitTypeDef gpio;
    gpio.GPIO_Pin=GPIO_Pin_1; gpio.GPIO_Mode=mode; gpio.GPIO_Speed=GPIO_Speed_2MHz;
    GPIO_Init(GPIOA,&gpio);
}

uint8_t DHT11_Decode(const uint8_t b[5], int16_t *temperature, uint16_t *humidity)
{
    int16_t t;
    uint16_t h;
    if ((uint8_t)(b[0]+b[1]+b[2]+b[3]) != b[4]) return 1;
    if (b[1] > 9 || (b[3] & 0x7F) > 9) return 2;
    h = b[0]*10U + b[1];
    t = (int16_t)(b[2]*10 + (b[3] & 0x7F));
    if (b[3] & 0x80) t = -t;
    if (h > 1000 || t < -200 || t > 600) return 2;
    *temperature = t; *humidity = h; return 0;
}

void DHT11_Init(uint32_t now)
{
    EXTI_InitTypeDef exti;
    NVIC_InitTypeDef irq;
    memset(&data,0,sizeof(data));state=0;receiving=rx_done=0;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);
    GPIO_SetBits(GPIOA, GPIO_Pin_1);
    data_mode(GPIO_Mode_IPU); /* External 4.7k still recommended for a bare sensor. */
    GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource1);
    exti.EXTI_Line=EXTI_Line1; exti.EXTI_Mode=EXTI_Mode_Interrupt;
    /* SPL configures trigger registers only when LineCmd is ENABLE. */
    exti.EXTI_Trigger=EXTI_Trigger_Falling; exti.EXTI_LineCmd=ENABLE;
    EXTI_Init(&exti);
    EXTI->IMR &= ~EXTI_Line1; /* Keep capture masked until the start pulse ends. */
    EXTI_ClearITPendingBit(EXTI_Line1);
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    irq.NVIC_IRQChannel=EXTI1_IRQn; irq.NVIC_IRQChannelPreemptionPriority=1;
    irq.NVIC_IRQChannelSubPriority=0; irq.NVIC_IRQChannelCmd=ENABLE; NVIC_Init(&irq);
    next_read=now+2000; data.error=0;
}

void EXTI1_IRQHandler(void)
{
    uint16_t stamp, span;
    uint8_t bit;
    if (EXTI_GetITStatus(EXTI_Line1) == RESET) return;
    EXTI_ClearITPendingBit(EXTI_Line1);
    if (!receiving) return;
    stamp=delay_micros16(); span=(uint16_t)(stamp-edge_time); edge_time=stamp;
    if (!edge_count) { edge_count=1; return; }
    if (edge_count==1) {
        if (span < 120 || span > 210) { receiving=0; rx_done=2; }
        else edge_count=2;
        return;
    }
    if (span < 55 || span > 150) { receiving=0; rx_done=2; return; }
    bit=edge_count-2;
    rx[bit/8]=(uint8_t)((rx[bit/8]<<1) | (span >= 100));
    if (++edge_count==42) { receiving=0; rx_done=1; EXTI->IMR &= ~EXTI_Line1; }
}

void DHT11_Task(uint32_t now)
{
    uint8_t bytes[5], k, result;
    if (!state && (int32_t)(now-next_read)>=0) {
        EXTI->IMR &= ~EXTI_Line1; receiving=0; rx_done=0;
        data.idle_high=(GPIOA->IDR & GPIO_Pin_1)!=0;
        data.attempts++;
        data_mode(GPIO_Mode_Out_OD);
        GPIO_ResetBits(GPIOA,GPIO_Pin_1);
        deadline=now+20; next_read=now+2500; state=1;
    } else if (state==1 && (int32_t)(now-deadline)>=0) {
        memset((void *)rx,0,sizeof(rx)); edge_count=rx_done=0;
        EXTI_ClearITPendingBit(EXTI_Line1);
        receiving=1; EXTI->IMR |= EXTI_Line1;
        GPIO_SetBits(GPIOA,GPIO_Pin_1);
        data_mode(GPIO_Mode_IPU); /* Release and explicitly enable the input path. */
        deadline=now+10; state=2;
    } else if (state==2 && (rx_done || (int32_t)(now-deadline)>=0)) {
        EXTI->IMR &= ~EXTI_Line1; receiving=0;
        data.edges=edge_count;
        data.frame_received=rx_done==1;
        if (rx_done==1) {
            for(k=0;k<5;k++){bytes[k]=rx[k];data.bytes[k]=bytes[k];}
            result=DHT11_Decode(bytes,&data.temperature_x10,&data.humidity_x10);
        } else result=rx_done ? 4 : (data.idle_high ? 3 : 5);
        data.error=result;
        if (!result) { data.valid=1; data.last_good_ms=now; }
        state=0;
    }
    if (data.valid && (uint32_t)(now-data.last_good_ms)>6000) data.valid=0;
}
const DHT11_Data *DHT11_Get(void) { return &data; }
uint8_t DHT11_Busy(void) { return state != 0; }
