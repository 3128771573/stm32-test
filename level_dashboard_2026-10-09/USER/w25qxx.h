#ifndef W25QXX_H
#define W25QXX_H
#include "stm32f10x.h"
/* Standard 3.3V W25Q20..128, SPI1 PA5/6/7, CS PA4. No implicit unprotect/format. */
uint8_t W25Q_Init(void);
uint32_t W25Q_ID(void);
uint32_t W25Q_Capacity(void);
uint8_t W25Q_Read(uint32_t address, uint8_t *data, uint16_t size);
uint8_t W25Q_ProgramStart(uint32_t address, const uint8_t *data, uint16_t size);
uint8_t W25Q_EraseStart(uint32_t address);
uint8_t W25Q_Busy(void); /* 0 ready, 1 busy, 2 communication error */
#endif
