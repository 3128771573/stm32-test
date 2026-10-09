#ifndef LOGGER_H
#define LOGGER_H
#include "stm32f10x.h"
#define LOGGER_SLOTS 2048
#define LOGGER_RECENT 32
#define LOG_ENV_VALID 1
#define LOG_LEVEL_VALID 2
enum { LOG_OFFLINE, LOG_SCANNING, LOG_READY, LOG_WRITING, LOG_ERROR, LOG_FOREIGN };
typedef struct {
    uint32_t sequence, uptime_s;
    int16_t temperature_x10, pitch_x10, roll_x10;
    uint16_t humidity_x10;
    uint8_t flags;
} LogRecord;
void Logger_Init(uint32_t now);
void Logger_Task(uint32_t now);
uint8_t Logger_Save(const LogRecord *record);
uint8_t Logger_State(void);
uint16_t Logger_Count(void);
uint8_t Logger_RecentCount(void);
uint32_t Logger_LastSequence(void);
uint8_t Logger_ReadRecent(uint8_t rank, LogRecord *record);
uint8_t Logger_ScanProgress(void);
uint8_t Logger_Decode(const uint8_t raw[32], LogRecord *record);
#endif
