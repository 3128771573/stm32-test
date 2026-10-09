#ifndef DASHBOARD_H
#define DASHBOARD_H
#include "stm32f10x.h"
#define DASHBOARD_PAGE_COUNT 7
#define POSE_OK 0
#define POSE_FLIPPED 1
#define POSE_RANGE 2
typedef struct {
    uint8_t page, browsing, rank, mpu_valid, calibrating, cal_error, cal_percent, level;
    int32_t pitch_x10, roll_x10, pitch_text, roll_text;
    uint32_t uptime_s;
    const char *toast;
    int16_t accel_raw[3],gyro_raw[3],temperature_raw;
    uint8_t raw_valid,pose_error;
    uint16_t sample_hz,frame_hz;
    uint32_t mpu_errors;
    uint32_t now_ms;
} DashboardView;
void Dashboard_Draw(const DashboardView *view);
void Dashboard_Init(void);
void Dashboard_TrendFeed(uint32_t now,int32_t pitch,int32_t roll,uint8_t valid);
#endif
