#ifndef EVENTLOG_H
#define EVENTLOG_H
#include "stm32f10x.h"

enum { EVENT_OFFLINE, EVENT_SCANNING, EVENT_READY, EVENT_ERASING,
       EVENT_VERIFY, EVENT_BODY, EVENT_COMMIT, EVENT_LOCKED, EVENT_ERROR };
enum { EVENT_BOOT=1, EVENT_ALIVE=2 };
typedef struct {
    uint32_t sequence,uptime_seconds,boot_count;
    uint8_t kind,reset_code;
} EventLast;

void EventLog_Init(uint32_t now,uint8_t reset_code);
void EventLog_Task(uint32_t now,uint32_t uptime_seconds);
uint8_t EventLog_State(void);
uint8_t EventLog_Busy(void);
uint32_t EventLog_BootCount(void);
uint8_t EventLog_BootRecorded(void);
uint16_t EventLog_SessionCount(void);
uint8_t EventLog_ReadSession(uint8_t rank,EventLast *event);
uint8_t EventLog_ScanProgress(void);
#endif
