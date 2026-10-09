#include "dashboard.h"
#include "oled.h"
#include "dht11.h"
#include "logger.h"
#include "w25qxx.h"
#include "controls.h"
#include <stdio.h>
#include <string.h>

#define TREND_POINTS 120
static int16_t trend_pitch[TREND_POINTS],trend_roll[TREND_POINTS];
static uint8_t trend_valid[TREND_POINTS],trend_head,trend_count,trend_started;
static uint32_t trend_next;
static int32_t bubble_x,bubble_y;
static int8_t bubble_draw_x,bubble_draw_y;
static uint8_t bubble_ready;
static uint32_t bubble_time;

void Dashboard_Init(void)
{
    trend_head=trend_count=trend_started=0;trend_next=0;
    bubble_ready=0;
}
static void trend_store(int32_t pitch,int32_t roll,uint8_t valid)
{
    if(pitch>3000)pitch=3000;else if(pitch< -3000)pitch=-3000;
    if(roll>3000)roll=3000;else if(roll< -3000)roll=-3000;
    trend_pitch[trend_head]=(int16_t)pitch;trend_roll[trend_head]=(int16_t)roll;
    trend_valid[trend_head]=valid;
    trend_head=(uint8_t)((trend_head+1)%TREND_POINTS);
    if(trend_count<TREND_POINTS)trend_count++;
}
void Dashboard_TrendFeed(uint32_t now,int32_t pitch,int32_t roll,uint8_t valid)
{
    uint32_t steps,k;
    if(!trend_started){trend_started=1;trend_next=now;}
    if((int32_t)(now-trend_next)<0)return;
    steps=(now-trend_next)/100+1;
    /* Preserve time gaps without drawing a fictitious line through missing data. */
    for(k=1;k<steps&&k<TREND_POINTS;k++)trend_store(0,0,0);
    trend_store(pitch,roll,valid);trend_next+=steps*100;
}

static const char *flash_state(uint8_t state)
{
    switch(state) {
        case LOG_SCANNING:return "SCAN";case LOG_READY:return "READY";
        case LOG_WRITING:return "SAVING";case LOG_ERROR:return "ERROR";
        case LOG_FOREIGN:return "LOCKED";default:return W25Q_ID()!=0 && W25Q_ID()!=0xFFFFFF ? "UNSUP" : "ABSENT";
    }
}
static uint32_t root(uint32_t n)
{
    uint32_t result=0,bit=1UL<<30;
    while(bit>n)bit>>=2;
    while(bit){if(n>=result+bit){n-=result+bit;result=(result>>1)+bit;}else result>>=1;bit>>=2;}
    return result;
}
static void fixed(char *buf,int32_t value,uint8_t sign)
{
    uint32_t mag=(uint32_t)(value<0 ? -value : value);
    if(sign) sprintf(buf,"%c%lu.%lu",value<0?'-':'+',(unsigned long)(mag/10),(unsigned long)(mag%10));
    else sprintf(buf,"%s%lu.%lu",value<0?"-":"",(unsigned long)(mag/10),(unsigned long)(mag%10));
}
static void large_fixed(char *buf,int32_t value)
{
    char number[16];uint8_t length,padding;
    fixed(number,value,0);length=(uint8_t)strlen(number);
    if(length>5){strcpy(buf," OVER");return;}
    /* Five character slots keep the decimal point in the fourth column. */
    padding=(uint8_t)(5-length);memset(buf,' ',padding);strcpy(buf+padding,number);
}
static void bubble_target(int32_t pitch,int32_t roll,int32_t *x,int32_t *y)
{
    uint32_t n,d;
    if(pitch>3000)pitch=3000;else if(pitch< -3000)pitch=-3000;
    if(roll>3000)roll=3000;else if(roll< -3000)roll=-3000;
    n=(uint32_t)(pitch*pitch+roll*roll);d=root(n);
    if(d*d<n)d++; /* Round up so the target remains strictly inside the bowl. */
    if(d<150)d=150;
    *x=roll*20L*256/(int32_t)d;*y=-pitch*20L*256/(int32_t)d;
}
static int8_t bubble_pixel(int32_t value)
{
    return (int8_t)(value>=0?(value+128)/256:-((-value+128)/256));
}
static void bubble_update(const DashboardView *v)
{
    int32_t x,y,delta;uint32_t elapsed;
    bubble_target(v->pitch_x10,v->roll_x10,&x,&y);
    if(!bubble_ready){
        bubble_x=x;bubble_y=y;bubble_ready=1;
        bubble_draw_x=bubble_pixel(x);bubble_draw_y=bubble_pixel(y);
    }else{
        elapsed=v->now_ms-bubble_time;if(elapsed>100)elapsed=100;
        bubble_x+=(x-bubble_x)*(int32_t)elapsed/(int32_t)(elapsed+45);
        bubble_y+=(y-bubble_y)*(int32_t)elapsed/(int32_t)(elapsed+45);
        delta=bubble_x-(int32_t)bubble_draw_x*256;
        if(delta>160||delta< -160)bubble_draw_x=bubble_pixel(bubble_x);
        delta=bubble_y-(int32_t)bubble_draw_y*256;
        if(delta>160||delta< -160)bubble_draw_y=bubble_pixel(bubble_y);
    }
    bubble_time=v->now_ms;
}
static void center_small(uint8_t y,const char *text)
{
    uint8_t width=(uint8_t)(strlen(text)*6);
    OLED_ShowSmallString(width<128?(128-width)/2:0,y,text);
}
static void level_page(const DashboardView *v)
{
    uint8_t x,y;char text[16];
    if(!v->mpu_valid) {
        bubble_ready=0;
        center_small(9,"LEVEL");
        center_small(28,v->calibrating?"HOLD STILL":v->cal_error?"PLACE FLAT":
            v->pose_error==POSE_FLIPPED?"FLIP BOARD":v->pose_error==POSE_RANGE?"TILT RANGE":"CHECK SENSOR");
        if(v->calibrating) {
            uint8_t percent=v->cal_percent>100?100:v->cal_percent;
            OLED_DrawLine(24,45,103,45,1);
            if(percent)OLED_FillRect(24,44,(uint8_t)(24+percent*79U/100U),46,1);
        }
        return;
    }
    bubble_update(v);x=(uint8_t)(31+bubble_draw_x);y=(uint8_t)(32+bubble_draw_y);
    OLED_DrawCircle(31,32,28,1);
    /* Four small target brackets instead of a cross through the bubble. */
    OLED_DrawLine(23,24,26,24,1);OLED_DrawLine(23,24,23,27,1);
    OLED_DrawLine(36,24,39,24,1);OLED_DrawLine(39,24,39,27,1);
    OLED_DrawLine(23,40,26,40,1);OLED_DrawLine(23,37,23,40,1);
    OLED_DrawLine(36,40,39,40,1);OLED_DrawLine(39,37,39,40,1);
    OLED_DrawDisc(x,y,7,0);OLED_DrawDisc(x,y,6,1);OLED_DrawDisc(x,y,4,0);
    OLED_DrawLine(x-2,y-2,x,y-2,1);OLED_DrawPoint(x-2,y-1,1);
    OLED_ShowSmallString(68,1,v->level?"LEVEL":"TILT");
    OLED_ShowSmallString(68,11,"PITCH");large_fixed(text,v->pitch_text);
    OLED_ShowLargeString(66,20,v->pitch_text>999||v->pitch_text< -999?" OVER":text);
    OLED_ShowSmallString(68,38,"ROLL");large_fixed(text,v->roll_text);
    OLED_ShowLargeString(66,47,v->roll_text>999||v->roll_text< -999?" OVER":text);
}
static void climate_page(void)
{
    const DHT11_Data *d=DHT11_Get();char text[16];
    OLED_ShowSmallString(6,17,"TEMP");OLED_ShowSmallString(74,17,"HUMID");
    if(d->valid) {
        large_fixed(text,d->temperature_x10);OLED_ShowLargeString(3,31,text);
        large_fixed(text,d->humidity_x10);OLED_ShowLargeString(68,31,text);
    } else {OLED_ShowLargeString(15,31,"--");OLED_ShowLargeString(83,31,"--");}
    OLED_ShowSmallString(8,52,"C");OLED_ShowSmallString(76,52,"%RH");
    if(d->error)OLED_ShowSmallString(98,1,d->valid?"OLD":"WAIT");
}
static void history_page(const DashboardView *v)
{
    LogRecord r;char text[40],a[16],b[16];
    if(!Logger_RecentCount()) {
        OLED_ShowLargeString(4,20,"NO RECORDS");
        OLED_ShowSmallString(10,42,flash_state(Logger_State()));return;
    }
    if(Logger_ReadRecent(v->rank,&r)) {
        OLED_ShowLargeString(4,22,Logger_State()==LOG_ERROR?"READ ERROR":"READING");return;
    }
    sprintf(text,"%u/%u",v->rank+1,Logger_RecentCount());OLED_ShowSmallString(92,1,text);
    OLED_ShowSmallString(6,17,"TEMP C");OLED_ShowSmallString(74,17,"HUM %");
    if(r.flags&LOG_ENV_VALID){large_fixed(a,r.temperature_x10);large_fixed(b,r.humidity_x10);}
    else {strcpy(a,"--");strcpy(b,"--");}
    OLED_ShowLargeString(3,29,a);OLED_ShowLargeString(68,29,b);
    if(r.flags&LOG_LEVEL_VALID){fixed(a,r.pitch_x10,1);fixed(b,r.roll_x10,1);sprintf(text,"P%s R%s",a,b);}
    else sprintf(text,"LEVEL --");
    OLED_ShowSmallString(4,47,text);
    sprintf(text,"#%lu %luS",(unsigned long)r.sequence,(unsigned long)r.uptime_s);OLED_ShowSmallString(4,57,text);
    /* Capture time is boot uptime, not a wall clock. */
}
static void system_page(const DashboardView *v)
{
    char text[24];const DHT11_Data *d=DHT11_Get();uint32_t cap=W25Q_Capacity();
    sprintf(text,"UP %luS",(unsigned long)v->uptime_s);
    OLED_ShowSmallString((uint8_t)(124-strlen(text)*6),1,text);
    sprintf(text,"MPU %s",v->raw_valid?"OK":"WAIT");OLED_ShowSmallString(4,18,text);
    sprintf(text,"DHT %s",d->valid&&!d->error?"OK":"WAIT");OLED_ShowSmallString(68,18,text);
    OLED_ShowSmallString(4,32,"FLASH");OLED_ShowSmallString(68,32,flash_state(Logger_State()));
    sprintf(text,"%luKB",(unsigned long)(cap/1024));OLED_ShowSmallString(4,44,text);
    sprintf(text,"%u LOG",Logger_Count());OLED_ShowSmallString(68,44,text);
    sprintf(text,"ID %06lX",(unsigned long)W25Q_ID());OLED_ShowSmallString(4,56,text);
}
static void raw_page(const DashboardView *v)
{
    uint8_t k;char text[24];
    OLED_ShowSmallString(4,16,"ACC");OLED_ShowSmallString(68,16,"GYRO");
    for(k=0;k<3;k++){
        if(v->raw_valid)sprintf(text,"%c%6d",'X'+k,v->accel_raw[k]);
        else sprintf(text,"%c    --",'X'+k);
        OLED_ShowSmallString(4,(uint8_t)(27+k*10),text);
        if(v->raw_valid)sprintf(text,"%c%6d",'X'+k,v->gyro_raw[k]);
        else sprintf(text,"%c    --",'X'+k);
        OLED_ShowSmallString(68,(uint8_t)(27+k*10),text);
    }
    if(v->raw_valid)sprintf(text,"TEMP RAW %d",v->temperature_raw);
    else sprintf(text,"MPU DATA UNAVAILABLE");
    OLED_ShowSmallString(4,57,text);
}
static uint16_t trend_scale(void)
{
    static const uint16_t scales[7]={20,50,100,200,500,1000,3000};
    uint16_t peak=0,a;uint8_t k;
    for(k=0;k<trend_count;k++)if(trend_valid[k]){
        a=(uint16_t)(trend_pitch[k]<0?-trend_pitch[k]:trend_pitch[k]);if(a>peak)peak=a;
        a=(uint16_t)(trend_roll[k]<0?-trend_roll[k]:trend_roll[k]);if(a>peak)peak=a;
    }
    for(k=0;k<6&&peak>scales[k];k++);
    return scales[k];
}
static void trend_plot(const int16_t *values,uint8_t center,uint16_t scale)
{
    uint8_t k,index,x,y,old_x=0,old_y=0,connected=0;
    OLED_DrawLine(3,center-6,3,center+6,1);
    for(k=4;k<124;k+=8)OLED_DrawPoint(k,center,1);
    for(k=0;k<trend_count;k++){
        index=(uint8_t)((trend_head+TREND_POINTS-trend_count+k)%TREND_POINTS);
        x=(uint8_t)(124-trend_count+k);
        if(!trend_valid[index]){connected=0;continue;}
        y=(uint8_t)(center-(int32_t)values[index]*6/scale);
        if(connected)OLED_DrawLine(old_x,old_y,x,y,1);else OLED_DrawPoint(x,y,1);
        old_x=x;old_y=y;connected=1;
    }
}
static void trend_page(const DashboardView *v)
{
    char text[24],number[16];uint16_t scale=trend_scale();
    if(v->mpu_valid){fixed(number,v->pitch_x10,1);sprintf(text,"P %s",number);}
    else sprintf(text,"P --");
    OLED_ShowSmallString(4,13,text);
    sprintf(text,"+/-%uD",scale/10);OLED_ShowSmallString(80,1,text);
    if(v->mpu_valid){fixed(number,v->roll_x10,1);sprintf(text,"R %s",number);}
    else sprintf(text,"R --");
    OLED_ShowSmallString(4,38,text);
    OLED_ShowSmallString(104,38,"12S");
    trend_plot(trend_pitch,29,scale);trend_plot(trend_roll,55,scale);
}
static void debug_page(const DashboardView *v)
{
    const DHT11_Data *d=DHT11_Get();char text[40];
    if(d->frame_received)sprintf(text,"D:%02X %02X %02X %02X %02X",d->bytes[0],d->bytes[1],d->bytes[2],d->bytes[3],d->bytes[4]);
    else sprintf(text,"D:NO COMPLETE FRAME");
    OLED_ShowSmallString(4,14,text);
    sprintf(text,"E%u N%u P%u TRY%u",d->error,d->edges,d->idle_high,d->attempts%1000);OLED_ShowSmallString(4,23,text);
    sprintf(text,"R%4u G%4u B%4u",(unsigned)TIM1->CCR1,(unsigned)TIM1->CCR2,(unsigned)TIM1->CCR3);OLED_ShowSmallString(4,32,text);
    sprintf(text,"AB%u K%u S%u F%u",(unsigned)(GPIOB->IDR&3),Controls_ButtonDown(),v->sample_hz,v->frame_hz);OLED_ShowSmallString(4,41,text);
    sprintf(text,"MPU ERR %lu",(unsigned long)v->mpu_errors);OLED_ShowSmallString(4,53,text);
}
void Dashboard_Draw(const DashboardView *v)
{
    static const char *titles[DASHBOARD_PAGE_COUNT]={"LEVEL","CLIMATE","HISTORY","SYSTEM","RAW DATA","TREND","DEBUG"};
    uint8_t width;
    if(v->page>=DASHBOARD_PAGE_COUNT)return;
    OLED_Clear();
    if(v->page) {bubble_ready=0;OLED_ShowSmallString(4,1,v->page==2&&v->browsing?"BROWSE":titles[v->page]);}
    if(v->page==0) level_page(v);
    else if(v->page==1) climate_page();
    else if(v->page==2) history_page(v);
    else if(v->page==3)system_page(v);
    else if(v->page==4)raw_page(v);
    else if(v->page==5)trend_page(v);
    else debug_page(v);
    if(v->toast){
        if(!v->page)OLED_FillRect(64,0,127,8,0);
        width=(uint8_t)(strlen(v->toast)*6+4);if(width>124)width=124;
        OLED_FillRect(128-width,0,127,8,1);
        OLED_ShowSmallStringInverse(130-width,1,v->toast);
    }
}
