#include "w25qxx.h"
#include "delay.h"

static uint32_t jedec, capacity;
static uint8_t bus_error;
static void select_chip(uint8_t select) {
    if(select) GPIO_ResetBits(GPIOA,GPIO_Pin_4); else GPIO_SetBits(GPIOA,GPIO_Pin_4);
}
static uint8_t transfer(uint8_t value)
{
    uint16_t start=delay_micros16();
    if(bus_error) return 0xFF;
    while(!(SPI1->SR & SPI_I2S_FLAG_TXE))
        if((uint16_t)(delay_micros16()-start)>1000) {bus_error=1;return 0xFF;}
    SPI1->DR=value;
    while(!(SPI1->SR & SPI_I2S_FLAG_RXNE))
        if((uint16_t)(delay_micros16()-start)>1000) {bus_error=1;return 0xFF;}
    return (uint8_t)SPI1->DR;
}
static uint8_t status(void) {
    uint8_t v; select_chip(1); transfer(0x05); v=transfer(0xFF); select_chip(0); return v;
}
static void address24(uint32_t address) {
    transfer((uint8_t)(address>>16)); transfer((uint8_t)(address>>8)); transfer((uint8_t)address);
}
uint8_t W25Q_Init(void)
{
    GPIO_InitTypeDef gpio;
    SPI_InitTypeDef spi;
    uint8_t vendor,type,size;
    capacity=jedec=0; bus_error=0;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA|RCC_APB2Periph_SPI1,ENABLE);
    GPIO_SetBits(GPIOA,GPIO_Pin_4);
    gpio.GPIO_Pin=GPIO_Pin_4; gpio.GPIO_Mode=GPIO_Mode_Out_PP; gpio.GPIO_Speed=GPIO_Speed_50MHz;
    GPIO_Init(GPIOA,&gpio);
    gpio.GPIO_Pin=GPIO_Pin_5|GPIO_Pin_7; gpio.GPIO_Mode=GPIO_Mode_AF_PP; GPIO_Init(GPIOA,&gpio);
    gpio.GPIO_Pin=GPIO_Pin_6; gpio.GPIO_Mode=GPIO_Mode_IN_FLOATING; GPIO_Init(GPIOA,&gpio);
    SPI_StructInit(&spi); spi.SPI_Mode=SPI_Mode_Master;
    spi.SPI_NSS=SPI_NSS_Soft; spi.SPI_BaudRatePrescaler=SPI_BaudRatePrescaler_16;
    SPI_Init(SPI1,&spi); SPI_Cmd(SPI1,ENABLE);
    select_chip(1); transfer(0xAB); select_chip(0); delay_us(10);
    select_chip(1); transfer(0x9F); vendor=transfer(0xFF); type=transfer(0xFF); size=transfer(0xFF); select_chip(0);
    jedec=((uint32_t)vendor<<16)|((uint32_t)type<<8)|size;
    if(bus_error || vendor!=0xEF || (type!=0x40 && type!=0x70) || size<0x12 || size>0x18) return 1;
    capacity=1UL<<size;
    return 0;
}
uint32_t W25Q_ID(void) {return jedec;}
uint32_t W25Q_Capacity(void) {return capacity;}
uint8_t W25Q_Busy(void) {
    uint8_t v;
    if(!capacity || bus_error) return 2;
    v=status(); return bus_error || v==0xFF ? 2 : (v&1);
}
uint8_t W25Q_Read(uint32_t address,uint8_t *data,uint16_t size)
{
    uint16_t i;
    if(!capacity || !size || address>=capacity || size>capacity-address || W25Q_Busy()!=0) return 1;
    select_chip(1); transfer(0x03); address24(address);
    for(i=0;i<size;i++) data[i]=transfer(0xFF);
    select_chip(0); return bus_error;
}
static uint8_t write_enable(void) {
    uint8_t value;
    if(W25Q_Busy()!=0) return 1;
    select_chip(1); transfer(0x06); select_chip(0);
    value=status();return bus_error || !(value & 2);
}
uint8_t W25Q_ProgramStart(uint32_t address,const uint8_t *data,uint16_t size)
{
    uint16_t i;
    if(!capacity || !size || size>256 || (address&255)+size>256 || address>=capacity || size>capacity-address) return 1;
    if(write_enable()) return 1;
    select_chip(1); transfer(0x02); address24(address);
    for(i=0;i<size;i++) transfer(data[i]);
    select_chip(0); return bus_error;
}
uint8_t W25Q_EraseStart(uint32_t address)
{
    if(!capacity || address>=capacity || (address&4095) || write_enable()) return 1;
    select_chip(1); transfer(0x20); address24(address); select_chip(0); return bus_error;
}
