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
static uint8_t save_snapshot(uint32_t now,int32_t pitch,int32_t roll,uint8_t angle_valid)
{
    const DHT11_Data *d=DHT11_Get();LogRecord r;
    r.sequence=0;r.uptime_s=now/1000;r.flags=0;
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
static void navigate_turn(DashboardView *v,int8_t turn,uint32_t now,uint8_t count)
{
    int16_t selection;
    if(!turn)return;
    if(v->page==2&&v->browsing){
        selection=(int16_t)v->rank+turn;
        if(selection>=0&&selection<count){v->rank=(uint8_t)selection;return;}
        v->page=selection<0?1:3;v->browsing=0;v->rank=0;history_armed=0;
    }else{
        selection=(int16_t)v->page+turn;
        while(selection<0)selection+=DASHBOARD_PAGE_COUNT;
        v->page=(uint8_t)(selection%DASHBOARD_PAGE_COUNT);v->rank=v->browsing=0;
        history_armed=v->page==2;history_since=now;
    }
}
static uint8_t history_pause(DashboardView *v,uint32_t now,uint8_t count)
{
    if(!history_armed||v->page!=2||v->browsing||!count||
        (uint32_t)(now-history_since)<HISTORY_PAUSE_MS)return 0;
    v->browsing=1;history_armed=0;return 1;
}
int main(void)
{
    uint8_t oled_ok,mpu_ok,sensor_ready,led_active=0,stale=1,valid,button,save_pending=0;
    uint8_t pose_valid=0,pose_error=POSE_OK;
    int8_t turn;
    int16_t acc[3],gyro[3],temp_raw;
    uint8_t k;
    uint16_t rate_samples=0,rate_frames=0;
    int32_t pitch=0,roll=0,px100,rx100,pitch_text=0,roll_text=0;
    uint32_t now,sample_next,frame_next,last_good,retry_next,heartbeat_next,led_until=0;
    uint32_t log_next,toast_until=0,save_seq=0;
    uint32_t rate_since;
    const char *toast=0;
    DashboardView view;
    memset(&view,0,sizeof(view));Dashboard_Init();
    Buzzer_Init();LED_Init();Controls_Init();delay_init();Buzzer_Beep(BOOT_BEEP_MS);
    SoftI2C_Init();SoftI2C_BusRecover();delay_ms(300);
    oled_ok=SoftI2C_Probe(OLED_ADDR7);mpu_ok=SoftI2C_Probe(MPU6050_ADDR7);
    if(!oled_ok){while(1){uint8_t i;for(i=0;i<(mpu_ok?3:5);i++)
        {LED_On();delay_ms(80);LED_Off();delay_ms(120);}delay_ms(1200);}}
    OLED_Init();
    now=delay_millis();DHT11_Init(now);Logger_Init(now);
    sensor_ready=MPU6050_Init()==0;
    now=delay_millis();
    if(sensor_ready)cal_begin(now);
    sample_next=frame_next=last_good=now;retry_next=now+1000;heartbeat_next=now+1000;log_next=now+60000;
    rate_since=last_activity=now;idle_pitch=idle_roll=0;
    view.page=view.browsing=view.rank=0;
    while(1){
        now=delay_millis();DHT11_Task(now);
        if(beep_active&&due(now,beep_until)){Buzzer_Off();beep_active=0;}
        if(led_active&&due(now,led_until)){LED_Off();led_active=0;}
        if(due(now,heartbeat_next)){LED_On();led_active=1;led_until=now+30;heartbeat_next=now+1000;}
        turn=Controls_TakeTurn();button=Controls_TakeButton();
        if(button&CONTROL_HOME){view.page=view.browsing=view.rank=history_armed=0;frame_next=now;
            toast="HOME";toast_until=now+1500;}
        if(turn){
            navigate_turn(&view,turn,now,Logger_RecentCount());
            frame_next=now;
        }
        if(button&CONTROL_CLICK){
            if(view.page==2){
                if(Logger_RecentCount()){view.browsing=!view.browsing;history_armed=0;toast=0;}
                else{toast="NO RECORDS";toast_until=now+2000;}
            }else{
                if(!save_snapshot(now,pitch,roll,calibrated&&!stale&&pose_valid)){
                    toast="SAVING";save_pending=1;save_seq=Logger_LastSequence();
                }else toast=Logger_State()==LOG_OFFLINE?"NO FLASH":Logger_State()==LOG_FOREIGN?"LOCKED":"NOT READY";
                toast_until=now+2000;
            }
            frame_next=now;
        }
        if(history_pause(&view,now,Logger_RecentCount()))frame_next=now;
        if(due(now,sample_next)){
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
        if(due(now,log_next)&&Logger_State()==LOG_READY&&DHT11_Get()->valid&&!DHT11_Get()->error){
            if(!save_snapshot(now,pitch,roll,calibrated&&!stale&&pose_valid))log_next=now+60000;
        }
        if(save_pending){
            if(Logger_LastSequence()!=save_seq){toast="SAVED";toast_until=now+1500;save_pending=0;}
            else if(Logger_State()==LOG_ERROR){toast="SAVE ERROR";toast_until=now+3000;save_pending=0;}
        }
        if(toast&&due(now,toast_until))toast=0;
        screen_activity(now,turn||button||Controls_ButtonDown(),
            calibrated&&!stale&&pose_valid&&!cal_active&&!toast&&
            !(view.page==1&&DHT11_Get()->error)&&
            !((view.page==2||view.page==3)&&(Logger_State()==LOG_ERROR||Logger_State()==LOG_FOREIGN)),
            pitch,roll);
        if(view.rank>=Logger_RecentCount())view.rank=0;
        if(Logger_State()==LOG_WRITING)Controls_SetLight(LIGHT_SAVE);
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
            view.uptime_s=now/1000;view.toast=Controls_ButtonDown()?"KEY DOWN":toast;
            view.now_ms=now;
            Dashboard_Draw(&view);OLED_StartRefresh();rate_frames++;
            frame_next=now+(view.page==0?LEVEL_DISPLAY_INTERVAL_MS:DISPLAY_INTERVAL_MS);
        }
        if(!DHT11_Busy())OLED_Service(now);
        OLED_RefreshStep();
    }
}
