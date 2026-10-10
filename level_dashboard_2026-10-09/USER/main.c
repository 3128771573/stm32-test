#include "stm32f10x.h"
#include "delay.h"
#include "led.h"
#include "i2c.h"
#include "oled.h"
#include "mpu6050.h"
#include "buzzer.h"
#include "controls.h"
#include "dht11.h"
#include "logger.h"
#include "dashboard.h"
#include "w25qxx.h"
#include "eventlog.h"
#include <string.h>

/* 中断只计时；共用 I2C 的采样和短块刷新均在主循环执行。 */
#define CAL_SAMPLES 100
#define CAL_MIN_GOOD 90
#define CAL_MOTION_LSB 1200
#define MIN_CAL_Z 8000
#define SAMPLE_INTERVAL_MS 5
#define DISPLAY_INTERVAL_MS 50
#define LEVEL_DISPLAY_INTERVAL_MS 33
#define IDLE_DIM_MS 45000UL
#define HISTORY_PAUSE_MS 800UL
#define LPF_SHIFT 4
#define LPF_FAST_SHIFT 2
#define LPF_FAST_LSB 600
#define READ_TIMEOUT_MS 100
#define LEVEL_ENTER_X10 10
#define LEVEL_EXIT_X10 20
#define LEVEL_HOLD_MS 100
#define LEVEL_EXIT_MS 100
#define BEEP_MS 100
#define BOOT_BEEP_MS 60
#define PITCH_DIR (-1)
#define ROLL_DIR (1)
#define BUBBLE_X_DIR (1)
#define BUBBLE_Y_DIR (1)
#define HUD_MAX_OFFSET 23
#define HUD_FS_X10 150

static int32_t lpf[3]; /* Q8，乘法避免左移负数 */
static int32_t base_ax, base_ay, base_az;
static uint8_t filter_valid, level_latched, inside_active, outside_active, beep_active;
static uint32_t inside_since, outside_since, beep_until;

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }
static uint8_t due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }
static const char *reset_reason(uint32_t flags)
{
    if(flags&RCC_CSR_IWDGRSTF)return "IWD";
    if(flags&RCC_CSR_WWDGRSTF)return "WWD";
    if(flags&RCC_CSR_SFTRSTF)return "SW";
    if(flags&RCC_CSR_PORRSTF)return "POR";
    if(flags&RCC_CSR_LPWRRSTF)return "LPW";
    if(flags&RCC_CSR_PINRSTF)return "PIN";
    return "UNK";
}
static void filter_feed(const int16_t *acc)
{
    uint8_t k, shift = LPF_SHIFT;
    /* 三轴使用同一速度，快速转动时保持轨迹方向。 */
    if (filter_valid)
        for (k = 0; k < 3; k++)
            if (abs32((int32_t)acc[k] * 256 - lpf[k]) > LPF_FAST_LSB * 256L)
                shift = LPF_FAST_SHIFT;
    for (k = 0; k < 3; k++) {
        int32_t target = (int32_t)acc[k] * 256;
        if (!filter_valid) lpf[k] = target;
        else lpf[k] += (target - lpf[k]) / (1L << shift);
    }
    filter_valid = 1;
}

static void beep_start(uint32_t now, uint16_t duration)
{
    if (!duration) return;
    Buzzer_On(); beep_active = 1; beep_until = now + duration;
}
static void level_reset(void)
{
    level_latched = inside_active = outside_active = beep_active = 0;
    Buzzer_Off();
}
static void level_update(uint32_t now, int32_t pitch, int32_t roll)
{
    uint8_t inside = abs32(pitch) <= LEVEL_ENTER_X10 && abs32(roll) <= LEVEL_ENTER_X10;
    uint8_t outside = abs32(pitch) >= LEVEL_EXIT_X10 || abs32(roll) >= LEVEL_EXIT_X10;
    if (inside) {
        if (!inside_active) { inside_since = now; inside_active = 1; }
        if (!level_latched && (uint32_t)(now - inside_since) >= LEVEL_HOLD_MS) {
            level_latched = 1; beep_start(now, BEEP_MS);
        }
    } else inside_active = 0;
    if (outside) {
        if (!outside_active) { outside_since = now; outside_active = 1; }
        if (level_latched && (uint32_t)(now - outside_since) >= LEVEL_EXIT_MS)
            level_latched = 0;
    } else outside_active = 0;
}

/* 后台校准：页面切换和温湿度读取继续工作。 */
static uint8_t cal_active, calibrated, cal_error, cal_attempts, cal_good;
static int32_t cal_sum[3];
static int16_t cal_lo[3], cal_hi[3];
static uint32_t cal_next, cal_retry;
static void cal_begin(uint32_t now)
{
    uint8_t k;
    cal_active=1;cal_error=cal_attempts=cal_good=0;cal_next=now;
    for(k=0;k<3;k++){cal_sum[k]=0;cal_lo[k]=32767;cal_hi[k]=-32768;}
}
static void cal_feed(uint32_t now,const int16_t *acc,uint8_t valid)
{
    uint8_t k,ok;
    if(!cal_active || !due(now,cal_next))return;
    cal_next=now+10;cal_attempts++;
    if(valid){cal_good++;for(k=0;k<3;k++){
        cal_sum[k]+=acc[k];if(acc[k]<cal_lo[k])cal_lo[k]=acc[k];if(acc[k]>cal_hi[k])cal_hi[k]=acc[k];
    }}
    if(cal_attempts<CAL_SAMPLES)return;
    ok=cal_good>=CAL_MIN_GOOD;
    if(ok){for(k=0;k<3;k++)if((int32_t)cal_hi[k]-cal_lo[k]>CAL_MOTION_LSB)ok=0;
        if(cal_sum[2]/cal_good<MIN_CAL_Z)ok=0;}
    cal_active=0;
    if(ok){
        base_ax=cal_sum[0]/cal_good;base_ay=cal_sum[1]/cal_good;base_az=cal_sum[2]/cal_good;
        for(k=0;k<3;k++)lpf[k]=cal_sum[k]/cal_good*256;
        filter_valid=calibrated=1;level_reset();
    }else{cal_error=1;cal_retry=now+1000;}
}
static int32_t stable_text(int32_t raw_x100,int32_t previous)
{
    if(abs32(raw_x100-previous*10)<15)return previous;
    return raw_x100>=0?(raw_x100+5)/10:(raw_x100-5)/10;
}
static uint8_t pose_check(int16_t raw_z)
{
    /* X/Y alone cannot distinguish a level board from an upside-down board. */
    if(raw_z<0)return POSE_FLIPPED;
    if(raw_z<base_az/4||lpf[2]/256<base_az/4)return POSE_RANGE;
    return POSE_OK;
}
static uint8_t save_snapshot(uint32_t uptime_seconds,int32_t pitch,int32_t roll,uint8_t angle_valid)
{
    const DHT11_Data *d=DHT11_Get();LogRecord r;
    if(EventLog_Busy())return 1;
    r.sequence=0;r.uptime_s=uptime_seconds;r.flags=0;
    r.temperature_x10=d->temperature_x10;r.humidity_x10=d->humidity_x10;
    r.pitch_x10=(int16_t)pitch;r.roll_x10=(int16_t)roll;
    if(d->valid&&!d->error)r.flags|=LOG_ENV_VALID;
    if(angle_valid)r.flags|=LOG_LEVEL_VALID;
    return Logger_Save(&r);
}
static uint32_t last_activity;
static int32_t idle_pitch,idle_roll;
static void screen_activity(uint32_t now,uint8_t input,uint8_t allow_idle,int32_t pitch,int32_t roll)
{
    if(input||!allow_idle||abs32(pitch-idle_pitch)>=3||abs32(roll-idle_roll)>=3){
        last_activity=now;idle_pitch=pitch;idle_roll=roll;
    }
    OLED_SetIdle((uint32_t)(now-last_activity)>=IDLE_DIM_MS);
}
static uint32_t history_since;
static uint8_t history_armed;
static uint32_t strobe_since;
static uint8_t strobe_armed;
static uint32_t event_since;
static uint8_t event_armed;
static void navigate_turn(DashboardView *v,int8_t turn,uint32_t now,uint8_t count)
{
    int16_t selection;
    if(!turn)return;
    if(v->page==STROBE_PAGE&&v->browsing){
        selection=(int16_t)v->strobe_mode+turn;
        if(selection>=0&&selection<STROBE_MODE_COUNT){v->strobe_mode=(uint8_t)selection;return;}
        v->page=selection<0?6:RUNTIME_PAGE;v->browsing=0;v->rank=0;
        strobe_armed=history_armed=event_armed=0;
    }else if(v->page==2&&v->browsing){
        selection=(int16_t)v->rank+turn;
        if(selection>=0&&selection<count){v->rank=(uint8_t)selection;return;}
        v->page=selection<0?1:3;v->browsing=0;v->rank=0;history_armed=0;
    }else if(v->page==EVENT_PAGE&&v->browsing){
        selection=(int16_t)v->rank+turn;
        if(selection>=0&&selection<EventLog_SessionCount()){
            v->rank=(uint8_t)selection;return;
        }
        v->page=selection<0?RUNTIME_PAGE:0;v->browsing=0;v->rank=0;event_armed=0;
    }else{
        selection=(int16_t)v->page+turn;
        while(selection<0)selection+=DASHBOARD_PAGE_COUNT;
        v->page=(uint8_t)(selection%DASHBOARD_PAGE_COUNT);v->rank=v->browsing=0;
        history_armed=v->page==2;history_since=now;
        strobe_armed=v->page==STROBE_PAGE;strobe_since=now;
        event_armed=v->page==EVENT_PAGE;event_since=now;
    }
}
static uint8_t history_pause(DashboardView *v,uint32_t now,uint8_t count)
{
    if(!history_armed||v->page!=2||v->browsing||!count||
        (uint32_t)(now-history_since)<HISTORY_PAUSE_MS)return 0;
    v->browsing=1;history_armed=0;return 1;
}
static uint8_t strobe_pause(DashboardView *v,uint32_t now)
{
    if(!strobe_armed||v->page!=STROBE_PAGE||v->browsing||
        (uint32_t)(now-strobe_since)<HISTORY_PAUSE_MS)return 0;
    v->browsing=1;strobe_armed=0;return 1;
}
static uint8_t event_pause(DashboardView *v,uint32_t now)
{
    if(!event_armed||v->page!=EVENT_PAGE||v->browsing||!EventLog_SessionCount()||
       (uint32_t)(now-event_since)<HISTORY_PAUSE_MS)return 0;
    v->browsing=1;event_armed=0;return 1;
}

#define POST_STEP_COUNT 11
#define POST_DHT_STEP 5
#define POST_SCAN_STEP 7
#define POST_EVENT_STEP 10
static uint8_t post_dht_started,post_scan_started,post_event_started,post_timebase_ok;

static void post_pump(void)
{
    uint32_t now=delay_millis();
    if(post_dht_started)DHT11_Task(now);
    if(post_scan_started)Logger_Task(now);
    if(post_event_started)EventLog_Task(now,now/1000UL);
    if(!DHT11_Busy())OLED_Service(now);
    OLED_RefreshStep();
}

static uint8_t post_clock_check(void)
{
    RCC_ClocksTypeDef clocks;
    RCC_GetClocksFreq(&clocks);
    return (uint8_t)(SystemCoreClock==72000000UL&&
        clocks.SYSCLK_Frequency==72000000UL&&clocks.HCLK_Frequency==72000000UL&&
        clocks.PCLK1_Frequency==36000000UL&&clocks.PCLK2_Frequency==72000000UL);
}

static uint8_t post_systick_check(void)
{
    uint32_t before,after;
    volatile uint32_t guard=1000000UL;
    if((SysTick->CTRL&7UL)!=7UL||SysTick->LOAD+1UL!=SystemCoreClock/1000UL)return 0;
    before=delay_millis();
    while(delay_millis()==before&&guard)guard--;
    after=delay_millis();
    return (uint8_t)(after-before==1UL);
}

static uint8_t post_tim2_check(void)
{
    RCC_ClocksTypeDef clocks;
    uint32_t timer_clock;
    uint16_t start;
    volatile uint32_t guard=1000000UL;
    RCC_GetClocksFreq(&clocks);
    timer_clock=clocks.PCLK1_Frequency;
    if(clocks.PCLK1_Frequency!=clocks.HCLK_Frequency)timer_clock*=2UL;
    if(!(TIM2->CR1&TIM_CR1_CEN)||timer_clock<1000000UL||
        (timer_clock%1000000UL)||TIM2->PSC+1UL!=timer_clock/1000000UL)return 0;
    start=delay_micros16();
    while((uint16_t)(delay_micros16()-start)<100U&&guard)guard--;
    return (uint8_t)((uint16_t)(delay_micros16()-start)>=100U);
}

static uint8_t post_encoder_config(void)
{
    return (uint8_t)((GPIOB->CRL&0xFFUL)==0x88UL&&(GPIOB->ODR&3UL)==3UL);
}

static uint8_t post_outputs_config(void)
{
    uint32_t channels=TIM_CCER_CC1E|TIM_CCER_CC2E|TIM_CCER_CC3E;
    return (uint8_t)((GPIOA->CRH&0x0FFFUL)==0x0BBBUL&&
        (TIM1->CR1&TIM_CR1_CEN)&&(TIM1->BDTR&TIM_BDTR_MOE)&&
        (TIM1->CCER&channels)==channels&&TIM1->ARR+1UL==3600UL&&
        (TIM1->CCMR1&0x6868UL)==0x6868UL&&
        (TIM1->CCMR2&0x0068UL)==0x0068UL&&
        (GPIOB->CRH&0x0FUL)==0x03UL&&(GPIOB->ODR&GPIO_Pin_8));
}

static const char *post_names[POST_STEP_COUNT]={
    "CLOCK","SYSTICK","TIM2","OLED","MPU6050",
    "DHT11","W25QXX","LOG AREA","ENCODER","OUTPUTS",
    "EVENT"
};

static void post_draw(uint8_t current,uint8_t completed,char *results[POST_STEP_COUNT],
    const char *dht_detail,uint32_t flash_id)
{
    uint8_t i,page,progress,y;
    char id_text[7];
    char count_text[3],step_text[3];
    page=current==0xFFU?2:(uint8_t)(current/POST_DHT_STEP);
    OLED_Clear();
    OLED_FillRect(0,0,127,7,1);
    OLED_ShowSmallStringInverse(2,0,"STM32 SYSTEM POST");
    OLED_ShowSmallStringInverse(108,0,page==0?"1/3":page==1?"2/3":"3/3");
    if(current==0xFFU)OLED_ShowSmallString(0,10,
        post_timebase_ok?"CHECKS COMPLETE":"TIMEBASE FAULT");
    else{
        count_text[0]=(char)('0'+completed/10U);count_text[1]=(char)('0'+completed%10U);count_text[2]=0;
        step_text[0]=(char)('0'+((current+1U)/10U));step_text[1]=(char)('0'+((current+1U)%10U));step_text[2]=0;
        OLED_ShowSmallString(0,10,"CHECKS");
        OLED_ShowSmallString(42,10,count_text);
        OLED_ShowSmallString(54,10,"/11");
        OLED_ShowSmallString(84,10,"STEP");
        OLED_ShowSmallString(108,10,step_text);
    }
    OLED_DrawLine(0,18,127,18,1);
    for(i=0;i<5;i++){
        uint8_t step=(uint8_t)(i+page*POST_DHT_STEP);
        const char *detail="";
        if(step>=POST_STEP_COUNT)break;
        y=(uint8_t)(21U+i*8U);
        if(step==0)detail="72MHZ";
        else if(step==1)detail="1MS TICK";
        else if(step==2)detail="100US";
        else if(step==3)detail="I2C 3C";
        else if(step==4)detail="I2C CFG";
        else if(step==5)detail=dht_detail;
        else if(step==6){
            id_text[0]="0123456789ABCDEF"[(flash_id>>20)&15];
            id_text[1]="0123456789ABCDEF"[(flash_id>>16)&15];
            id_text[2]="0123456789ABCDEF"[(flash_id>>12)&15];
            id_text[3]="0123456789ABCDEF"[(flash_id>>8)&15];
            id_text[4]="0123456789ABCDEF"[(flash_id>>4)&15];
            id_text[5]="0123456789ABCDEF"[flash_id&15];id_text[6]=0;detail=id_text;
        }else if(step==7)detail="64KB READ";
        else if(step==8)detail="PB0 PB1";
        else if(step==9)detail="PWM+BUZ";
        else detail="8KB READ";
        OLED_ShowSmallString(0,y,post_names[step]);
        OLED_ShowSmallString(48,y,detail);
        OLED_ShowSmallString(99,y,results[step]);
        if(step==current)OLED_DrawRect(0,(uint8_t)(y-1U),127,(uint8_t)(y+7U),1);
    }
    progress=(uint8_t)((uint32_t)completed*100UL/POST_STEP_COUNT);
    if(current==POST_SCAN_STEP&&Logger_State()==LOG_SCANNING)
        progress=(uint8_t)(((uint32_t)completed*100UL+
            (uint32_t)Logger_ScanProgress())/POST_STEP_COUNT);
    if(current==POST_EVENT_STEP&&EventLog_State()==EVENT_SCANNING)
        progress=(uint8_t)(((uint32_t)completed*100UL+
            (uint32_t)EventLog_ScanProgress())/POST_STEP_COUNT);
    if(progress>100)progress=100;
    OLED_DrawRect(1,61,126,63,1);
    if(progress)OLED_FillRect(3,62,(uint8_t)(2UL+(uint32_t)progress*123UL/100UL),62,1);
}

static void post_present(uint8_t current,uint8_t completed,char *results[POST_STEP_COUNT],
    const char *dht_detail,uint32_t flash_id)
{
    while(OLED_RefreshBusy())post_pump();
    post_draw(current,completed,results,dht_detail,flash_id);
    OLED_StartRefresh();
    while(OLED_RefreshBusy())post_pump();
}

static uint8_t post_startup(uint8_t *sensor_ready,uint8_t reset_code)
{
    uint8_t completed=0,step,clock_ok,systick_ok,tim2_ok;
    uint8_t last_scan=0,scan_progress;
    uint32_t now,flash_id=0;
    uint16_t timer_last;
    uint32_t timer_elapsed=0;
    volatile uint32_t guard;
    const DHT11_Data *dht;
    char *results[POST_STEP_COUNT];
    char dht_error_text[4];
    const char *dht_detail="WAIT";
    static const char *pending="--";
    for(step=0;step<POST_STEP_COUNT;step++)results[step]=(char *)pending;
    *sensor_ready=0;
    post_dht_started=post_scan_started=post_event_started=post_timebase_ok=0;

    step=0;results[step]=(char *)"TEST";post_present(step,completed,results,dht_detail,flash_id);
    clock_ok=post_clock_check();results[step]=(char *)(clock_ok?"DONE":"ERR");completed++;
    post_present(step,completed,results,dht_detail,flash_id);

    step=1;results[step]=(char *)"TEST";post_present(step,completed,results,dht_detail,flash_id);
    systick_ok=post_systick_check();results[step]=(char *)(systick_ok?"DONE":"ERR");completed++;
    post_present(step,completed,results,dht_detail,flash_id);

    step=2;results[step]=(char *)"TEST";post_present(step,completed,results,dht_detail,flash_id);
    tim2_ok=post_tim2_check();results[step]=(char *)(tim2_ok?"DONE":"ERR");completed++;
    post_timebase_ok=(uint8_t)(clock_ok&&systick_ok&&tim2_ok);
    /* Start the DHT power-on interval while the OLED and MPU checks continue. */
    if(post_timebase_ok){DHT11_Init(delay_millis());post_dht_started=1;}
    post_present(step,completed,results,dht_detail,flash_id);

    step=3;results[step]=(char *)"TEST";post_present(step,completed,results,dht_detail,flash_id);
    results[step]=(char *)(SoftI2C_Probe(OLED_ADDR7)?"DONE":"ERR");completed++;
    post_present(step,completed,results,dht_detail,flash_id);

    step=4;results[step]=(char *)"TEST";post_present(step,completed,results,dht_detail,flash_id);
    if(post_timebase_ok){*sensor_ready=(uint8_t)(MPU6050_Init()==0);
        results[step]=(char *)(*sensor_ready?"DONE":"ERR");}
    else results[step]=(char *)"SKIP";
    completed++;post_present(step,completed,results,dht_detail,flash_id);

    step=5;results[step]=(char *)"WAIT";
    if(post_timebase_ok){
        now=delay_millis();
        post_present(step,completed,results,dht_detail,flash_id);
        dht=DHT11_Get();timer_last=delay_micros16();timer_elapsed=0;guard=30000000UL;
        while((!dht->attempts||DHT11_Busy())&&
            (uint32_t)(delay_millis()-now)<6500UL&&timer_elapsed<6500000UL&&guard){
            guard--;post_pump();
            {
                uint16_t timer_now=delay_micros16();
                timer_elapsed+=(uint16_t)(timer_now-timer_last);timer_last=timer_now;
            }
        }
        dht=DHT11_Get();
        if(dht->attempts&&!DHT11_Busy()){
            if(dht->valid&&!dht->error){results[step]=(char *)"DONE";dht_detail="CRC OK";}
            else{results[step]=(char *)"ERR";dht_error_text[0]='E';
                dht_error_text[1]=(char)('0'+(dht->error%10U));dht_error_text[2]=0;dht_detail=dht_error_text;}
        }else{results[step]=(char *)"TIME";dht_detail="NO FRAME";}
    }else{results[step]=(char *)"SKIP";dht_detail="NO TIMER";}
    completed++;post_present(step,completed,results,dht_detail,flash_id);

    step=6;results[step]=(char *)"TEST";post_present(step,completed,results,dht_detail,flash_id);
    if(post_timebase_ok){
        Logger_Init(delay_millis());flash_id=W25Q_ID();
        if(Logger_State()!=LOG_OFFLINE&&flash_id&&W25Q_Capacity()>=65536UL)
            results[step]=(char *)"DONE";
        else results[step]=(char *)"MISS";
    }else results[step]=(char *)"SKIP";
    completed++;post_present(step,completed,results,dht_detail,flash_id);

    step=7;last_scan=0;
    if(Logger_State()==LOG_SCANNING){
        results[step]=(char *)"SCAN";post_present(step,completed,results,dht_detail,flash_id);
        post_scan_started=1;
        guard=120000UL;
        while(Logger_State()==LOG_SCANNING&&guard){
            guard--;post_pump();scan_progress=Logger_ScanProgress();
            if(scan_progress>=last_scan+5U){last_scan=scan_progress;
                post_present(step,completed,results,dht_detail,flash_id);}
        }
        if(Logger_State()==LOG_READY)results[step]=(char *)"DONE";
        else if(Logger_State()==LOG_FOREIGN)results[step]=(char *)"LOCK";
        else if(Logger_State()==LOG_SCANNING)results[step]=(char *)"TIME";
        else results[step]=(char *)"ERR";
    }else if(Logger_State()==LOG_READY)results[step]=(char *)"DONE";
    else if(Logger_State()==LOG_FOREIGN)results[step]=(char *)"LOCK";
    else if(Logger_State()==LOG_OFFLINE)results[step]=(char *)"SKIP";
    else results[step]=(char *)"ERR";
    completed++;post_present(step,completed,results,dht_detail,flash_id);

    step=8;results[step]=(char *)"TEST";post_present(step,completed,results,dht_detail,flash_id);
    results[step]=(char *)(post_encoder_config()?"CFG":"ERR");completed++;
    post_present(step,completed,results,dht_detail,flash_id);

    step=9;results[step]=(char *)"TEST";post_present(step,completed,results,dht_detail,flash_id);
    results[step]=(char *)(post_outputs_config()?"CFG":"ERR");completed++;
    post_present(step,completed,results,dht_detail,flash_id);

    step=POST_EVENT_STEP;last_scan=0;
    if(Logger_State()==LOG_READY){
        results[step]=(char *)"SCAN";post_present(step,completed,results,dht_detail,flash_id);
        EventLog_Init(delay_millis(),reset_code);post_event_started=1;
        guard=120000UL;
        while(guard&&(EventLog_State()==EVENT_SCANNING||EventLog_Busy()||
            (EventLog_State()==EVENT_READY&&!EventLog_BootRecorded()))){
            guard--;post_pump();scan_progress=EventLog_ScanProgress();
            if(EventLog_State()==EVENT_SCANNING&&scan_progress>=last_scan+10U){
                last_scan=scan_progress;post_present(step,completed,results,dht_detail,flash_id);
            }
        }
        if(EventLog_BootRecorded())results[step]=(char *)"DONE";
        else if(EventLog_State()==EVENT_LOCKED)results[step]=(char *)"LOCK";
        else if(EventLog_State()==EVENT_ERROR)results[step]=(char *)"ERR";
        else if(EventLog_State()==EVENT_OFFLINE)results[step]=(char *)"SKIP";
        else results[step]=(char *)"TIME";
    }else results[step]=(char *)"SKIP";
    completed++;
    post_present(0xFFU,completed,results,dht_detail,flash_id);
    (void)Controls_TakeTurn();(void)Controls_TakeButton();
    return post_dht_started;
}

int main(void)
{
    uint8_t oled_ok,mpu_ok,sensor_ready,dht_ready,led_active=0,stale=1,valid,button,save_pending=0;
    uint8_t pose_valid=0,pose_error=POSE_OK;
    uint8_t flash_retry=0,event_init_attempted=0;
    int8_t turn;
    int16_t acc[3],gyro[3],temp_raw;
    uint8_t k;
    uint16_t rate_samples=0,rate_frames=0;
    int32_t pitch=0,roll=0,px100,rx100,pitch_text=0,roll_text=0;
    uint32_t now,sample_next,frame_next,last_good,retry_next,heartbeat_next,led_until=0;
    uint32_t uptime_last,uptime_fraction,uptime_seconds;
    uint32_t log_next,toast_until=0,save_seq=0,last_loop_start,last_sample_try,elapsed;
    uint32_t flash_retry_next=0,flash_backoff=5000;
    uint32_t rate_since;
    const char *toast=0;
    DashboardView view;
    uint32_t reset_flags=RCC->CSR;
    memset(&view,0,sizeof(view));Dashboard_Init();
    view.reset_reason=reset_reason(reset_flags);RCC_ClearFlag();
    Buzzer_Init();LED_Init();Controls_Init();delay_init();Buzzer_Beep(BOOT_BEEP_MS);
    SoftI2C_Init();SoftI2C_BusRecover();delay_ms(300);
    oled_ok=SoftI2C_Probe(OLED_ADDR7);mpu_ok=SoftI2C_Probe(MPU6050_ADDR7);
    while(!oled_ok){uint8_t i;for(i=0;i<(mpu_ok?3:5);i++)
        {LED_On();delay_ms(80);LED_Off();delay_ms(120);}
        delay_ms(1200);SoftI2C_BusRecover();oled_ok=SoftI2C_Probe(OLED_ADDR7);
        mpu_ok=SoftI2C_Probe(MPU6050_ADDR7);
    }
    OLED_Init();
    dht_ready=post_startup(&sensor_ready,(uint8_t)(reset_flags>>24));
    if(!post_timebase_ok){
        volatile uint32_t i;
        while(1){LED_On();for(i=0;i<250000UL;i++);LED_Off();for(i=0;i<1000000UL;i++);}
    }
    event_init_attempted=post_event_started;
    now=delay_millis();
    uptime_last=now;uptime_fraction=now%1000UL;uptime_seconds=now/1000UL;
    if(sensor_ready)cal_begin(now);
    sample_next=frame_next=last_good=last_loop_start=last_sample_try=now;
    retry_next=now+1000;heartbeat_next=now+1000;log_next=now+60000;
    rate_since=last_activity=now;idle_pitch=idle_roll=0;
    view.page=view.browsing=view.rank=0;
    view.strobe_mode=STROBE_DOUBLE;
    while(1){
        now=delay_millis();if(dht_ready)DHT11_Task(now);
        elapsed=now-uptime_last;uptime_last=now;uptime_fraction+=elapsed;
        if(uptime_fraction>=1000UL){uptime_seconds+=uptime_fraction/1000UL;
            uptime_fraction%=1000UL;}
        elapsed=now-last_loop_start;last_loop_start=now;
        if(elapsed>view.max_loop_ms)view.max_loop_ms=(uint16_t)(elapsed>999?999:elapsed);
        if(beep_active&&due(now,beep_until)){Buzzer_Off();beep_active=0;}
        if(led_active&&due(now,led_until)){LED_Off();led_active=0;}
        if(due(now,heartbeat_next)){LED_On();led_active=1;led_until=now+30;heartbeat_next=now+1000;}
        turn=Controls_TakeTurn();button=Controls_TakeButton();
        if(button&CONTROL_HOME){view.page=view.browsing=view.rank=history_armed=strobe_armed=event_armed=0;
            frame_next=now;
            toast="HOME";toast_until=now+1500;}
        if(turn){
            navigate_turn(&view,turn,now,Logger_RecentCount());
            frame_next=now;
        }
        if(button&CONTROL_CLICK){
            if(view.page==STROBE_PAGE){
                view.browsing=!view.browsing;strobe_armed=0;toast=0;
            }else if(view.page==EVENT_PAGE){
                if(EventLog_SessionCount()){view.browsing=!view.browsing;event_armed=0;toast=0;}
                else{toast="NO SESSIONS";toast_until=now+2000;}
            }else if(view.page==2){
                if(Logger_RecentCount()){view.browsing=!view.browsing;history_armed=0;toast=0;}
                else{toast="NO RECORDS";toast_until=now+2000;}
            }else{
                if(!save_snapshot(uptime_seconds,pitch,roll,calibrated&&!stale&&pose_valid)){
                    toast="SAVING";save_pending=1;save_seq=Logger_LastSequence();
                }else toast=EventLog_Busy()?"BUSY":Logger_State()==LOG_OFFLINE?"NO FLASH":
                    Logger_State()==LOG_FOREIGN?"LOCKED":"NOT READY";
                toast_until=now+2000;
            }
            frame_next=now;
        }
        if(history_pause(&view,now,Logger_RecentCount()))frame_next=now;
        if(strobe_pause(&view,now))frame_next=now;
        if(event_pause(&view,now))frame_next=now;
        if(due(now,sample_next)){
            elapsed=now-last_sample_try;last_sample_try=now;
            if(elapsed>view.max_sample_gap_ms)
                view.max_sample_gap_ms=(uint16_t)(elapsed>999?999:elapsed);
            sample_next=now+SAMPLE_INTERVAL_MS;
            valid=sensor_ready&&MPU6050_ReadAll(acc,gyro,&temp_raw)==0;
            if(valid){
                rate_samples++;
                for(k=0;k<3;k++){view.accel_raw[k]=acc[k];view.gyro_raw[k]=gyro[k];}
                view.temperature_raw=temp_raw;
                if(stale){filter_valid=0;stale=0;}
                filter_feed(acc);last_good=now;
                cal_feed(now,acc,1);
                if(calibrated){
                    pose_error=pose_check(acc[2]);pose_valid=pose_error==POSE_OK;
                    if(pose_valid){
                        px100=PITCH_DIR*((lpf[0]/256-base_ax)*5730L/base_az);
                        rx100=ROLL_DIR*((lpf[1]/256-base_ay)*5730L/base_az);
                        pitch=px100/10;roll=rx100/10;
                        pitch_text=stable_text(px100,pitch_text);roll_text=stable_text(rx100,roll_text);
                        Controls_SetLevelOffset(pitch,roll);
                        level_update(now,pitch,roll);
                    }else level_reset();
                }
            }else{if(sensor_ready)view.mpu_errors++;inside_active=outside_active=0;cal_feed(now,acc,0);}
        }
        if((uint32_t)(now-last_good)>=READ_TIMEOUT_MS){
            if(!stale)level_reset();
            stale=1;
            /* 避免 120ms MPU 复位跨越 DHT 的 20ms 起始脉冲。 */
            if(due(now,retry_next)&&!DHT11_Busy()){
                SoftI2C_BusRecover();sensor_ready=MPU6050_Init()==0;retry_next=delay_millis()+1000;
            }
        }
        if(!calibrated&&!cal_active&&sensor_ready&&!stale&&due(now,cal_retry))cal_begin(now);
        Dashboard_TrendFeed(now,pitch,roll,calibrated&&!stale&&pose_valid);
        if((uint32_t)(now-rate_since)>=1000){
            view.sample_hz=(uint16_t)((uint32_t)rate_samples*1000/(now-rate_since));
            view.frame_hz=(uint16_t)((uint32_t)rate_frames*1000/(now-rate_since));
            rate_samples=rate_frames=0;rate_since=now;
        }
        Logger_Task(now);
        if(!event_init_attempted&&Logger_State()==LOG_READY){
            EventLog_Init(now,(uint8_t)(reset_flags>>24));event_init_attempted=1;
        }
        if(event_init_attempted)EventLog_Task(now,uptime_seconds);
        if(due(now,log_next)&&Logger_State()==LOG_READY&&!EventLog_Busy()&&
           DHT11_Get()->valid&&!DHT11_Get()->error){
            if(!save_snapshot(uptime_seconds,pitch,roll,calibrated&&!stale&&pose_valid))log_next=now+60000;
        }
        if(save_pending){
            if(Logger_LastSequence()!=save_seq){toast="SAVED";toast_until=now+1500;save_pending=0;}
            else if(Logger_State()==LOG_ERROR){toast="SAVE ERROR";toast_until=now+3000;save_pending=0;}
        }
        if(Logger_State()==LOG_ERROR&&!flash_retry){
            flash_retry=1;flash_backoff=5000;flash_retry_next=now+flash_backoff;
        }
        if(flash_retry&&(Logger_State()==LOG_ERROR||Logger_State()==LOG_OFFLINE)&&
            due(now,flash_retry_next)&&!DHT11_Busy()){
            Logger_Init(now);flash_retry_next=delay_millis()+flash_backoff;
            if(flash_backoff<60000UL){flash_backoff*=2; if(flash_backoff>60000UL)flash_backoff=60000UL;}
            if(Logger_State()==LOG_SCANNING)flash_retry=0;
        }
        if(Logger_State()==LOG_READY||Logger_State()==LOG_FOREIGN)flash_retry=0;
        if(toast&&due(now,toast_until))toast=0;
        screen_activity(now,turn||button||Controls_ButtonDown(),
            view.page!=STROBE_PAGE&&calibrated&&!stale&&pose_valid&&!cal_active&&!toast&&
            !(view.page==1&&DHT11_Get()->error)&&
            !((view.page==2||view.page==3)&&(Logger_State()==LOG_ERROR||Logger_State()==LOG_FOREIGN)),
            pitch,roll);
        if(view.page==2&&view.rank>=Logger_RecentCount())view.rank=0;
        if(view.page==EVENT_PAGE&&view.rank>=EventLog_SessionCount())view.rank=0;
        if(view.page==STROBE_PAGE){Controls_SetStrobe(view.strobe_mode);Controls_SetLight(LIGHT_STROBE);}
        else if(Logger_State()==LOG_WRITING)Controls_SetLight(LIGHT_SAVE);
        else if((view.page==0&&(!calibrated||stale||!pose_valid)&&!cal_active)||(view.page==1&&DHT11_Get()->error)||
            ((view.page==2||view.page==3)&&(Logger_State()==LOG_ERROR||Logger_State()==LOG_FOREIGN||Logger_State()==LOG_OFFLINE)))
            Controls_SetLight(LIGHT_ERROR);
        else if(view.page==0&&calibrated&&!stale&&pose_valid)Controls_SetLight(LIGHT_OFFSET);
        else Controls_SetLight(level_latched?LIGHT_LEVEL:LIGHT_IDLE);
        if(!OLED_RefreshBusy()&&due(now,frame_next)){
            view.mpu_valid=calibrated&&!stale&&pose_valid;view.calibrating=cal_active;view.cal_error=cal_error;
            view.pose_error=stale?POSE_OK:pose_error;
            view.raw_valid=sensor_ready&&!stale;
            view.cal_percent=cal_attempts;view.level=level_latched;
            view.pitch_x10=pitch;view.roll_x10=roll;view.pitch_text=pitch_text;view.roll_text=roll_text;
            view.uptime_s=uptime_seconds;view.toast=Controls_ButtonDown()?"KEY DOWN":toast;
            view.now_ms=now;
            view.strobe_hint=(uint8_t)(view.page==STROBE_PAGE&&(uint32_t)(now-strobe_since)<2000UL);
            Dashboard_Draw(&view);OLED_StartRefresh();rate_frames++;
            frame_next=now+(view.page==0?LEVEL_DISPLAY_INTERVAL_MS:DISPLAY_INTERVAL_MS);
        }
        if(!DHT11_Busy())OLED_Service(now);
        OLED_RefreshStep();
    }
}
