#ifndef DHT11_H
#define DHT11_H
#include "stm32f10x.h"
typedef struct {
    int16_t temperature_x10;
    uint16_t humidity_x10;
    uint32_t last_good_ms;
    uint8_t valid, error;
    uint8_t edges, idle_high;
    uint16_t attempts;
    uint8_t bytes[5],frame_received;
} DHT11_Data;
void DHT11_Init(uint32_t now);
void DHT11_Task(uint32_t now);
const DHT11_Data *DHT11_Get(void);
uint8_t DHT11_Busy(void);
uint8_t DHT11_Decode(const uint8_t bytes[5], int16_t *temperature, uint16_t *humidity);
#endif
