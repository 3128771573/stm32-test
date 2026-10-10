#ifndef DASHBOARD_H
#define DASHBOARD_H
#include "stm32f10x.h"
#define DASHBOARD_PAGE_COUNT 10
#define STROBE_PAGE 7
#define RUNTIME_PAGE 8
#define EVENT_PAGE 9
#define POSE_OK 0
#define POSE_FLIPPED 1
#define POSE_RANGE 2
#define DASHBOARD_FW_VERSION "20261010E"
typedef struct {
    uint8_t page, browsing, rank, mpu_valid, calibrating, cal_error, cal_percent, level;
    int32_t pitch_x10, roll_x10, pitch_text, roll_text;
    uint32_t uptime_s;
    const char *toast;
    int16_t accel_raw[3],gyro_raw[3],temperature_raw;
    uint8_t raw_valid,pose_error,strobe_mode;
    uint16_t sample_hz,frame_hz;
    uint16_t max_sample_gap_ms,max_loop_ms;
    uint8_t strobe_hint;
    const char *reset_reason;
    uint32_t mpu_errors;
    uint32_t now_ms;
} DashboardView;
void Dashboard_Draw(const DashboardView *view);
void Dashboard_Init(void);
void Dashboard_TrendFeed(uint32_t now,int32_t pitch,int32_t roll,uint8_t valid);
#endif
