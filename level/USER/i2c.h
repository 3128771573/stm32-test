#ifndef __I2C_H
#define __I2C_H

#include "stm32f10x.h"

/*---------------------------------------------------------------------------
  软件 I2C（用 GPIO 模拟），OLED 和 MPU6050 共用这一条总线。

     SCL = PB6
     SDA = PB7

  电气要点：两个脚都配成【开漏输出】(GPIO_Mode_Out_OD)，
  靠模块上自带的 4.7k 上拉电阻把线拉高。
  OLED 模块和 MPU6050 模块各带一组 4.7k，并联后约 2.35k，100kHz 下没问题。
  STM32 在开漏输出模式下输入通道仍然有效，所以读 SDA 直接读 IDR 就行，
  不需要来回切换输入/输出模式，这是最省事也最不容易出错的做法。

  所有地址都用【7 位地址】传进来，函数内部会自动拼上读写位。
  注意别把模块上标的 8 位地址混进来：
     OLED     模块标 0x78（8 位写地址） -> 7 位是 0x3C
     MPU6050  模块标 0x68（7 位地址）   -> 7 位就是 0x68
---------------------------------------------------------------------------*/

#define I2C_PORT        GPIOB
#define I2C_SCL_PIN     GPIO_Pin_6
#define I2C_SDA_PIN     GPIO_Pin_7

/* 半周期延时循环次数，决定总线速度。
   72MHz、-O0 下，每减小 1，SCL 大约快 15kHz：
       20 -> 约 110kHz
        8 -> 约 250kHz    <- 当前取值
        5 -> 约 380kHz
   250kHz 是速度和稳妥的折中点：整屏刷新从 85ms 降到约 37ms。
   I2C 规范的快速模式上限是 400kHz，OLED 和 MPU6050 都支持。
   如果换成长杜邦线以后出现花屏或读数据出错，把它调回 12 到 20 试试。 */
#define I2C_DELAY_LOOPS 8

void    SoftI2C_Init(void);
void    SoftI2C_BusRecover(void);

uint8_t SoftI2C_Probe(uint8_t addr7);
uint8_t SoftI2C_WriteBytes(uint8_t addr7, const uint8_t *buf, uint16_t len);
uint8_t SoftI2C_ReadBytes(uint8_t addr7, uint8_t *buf, uint16_t len);

/* 流式写：适合"一次事务里连发很多字节"的场合（比如把 1024 字节显存整屏推给 OLED）。
   中间不会插重复起始条件，所以整屏刷新是一个连续事务，速度快且屏上不会闪。
     用法： SoftI2C_Begin(addr); SoftI2C_SendByte(...); ... ; SoftI2C_End();  */
void    SoftI2C_Begin(uint8_t addr7);
uint8_t SoftI2C_SendByte(uint8_t val);      /* 返回 0 = 从机应答 */
void    SoftI2C_End(void);

uint8_t SoftI2C_WriteReg(uint8_t addr7, uint8_t reg, uint8_t val);
uint8_t SoftI2C_ReadRegs(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len);
uint8_t SoftI2C_ReadReg(uint8_t addr7, uint8_t reg, uint8_t *val);

#endif
