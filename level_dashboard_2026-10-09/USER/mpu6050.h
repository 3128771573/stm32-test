#ifndef __MPU6050_H
#define __MPU6050_H

#include "stm32f10x.h"

/*---------------------------------------------------------------------------
  MPU6050 六轴传感器（GY-521 模块）

  接线：VCC->3.3V，GND->GND，SCL->PB6，SDA->PB7
        AD0 悬空即可（模块上已有 4.7k 下拉），所以地址固定 0x68

  本驱动只用【直接读寄存器】的方式，不碰 DMP。
  做气泡水平仪用不到 DMP：DMP 要往芯片里灌固件、要拉进一大坨库代码，
  而姿态角用加速度计直接算就够了（水平仪是静态应用）。

  量程固定 ±2g，对应 16384 LSB/g，所以水平静止时 az 应该接近 +16384。
---------------------------------------------------------------------------*/

#define MPU6050_ADDR7               0x68

#define MPU6050_REG_SMPLRT_DIV      0x19
#define MPU6050_REG_CONFIG          0x1A
#define MPU6050_REG_GYRO_CONFIG     0x1B
#define MPU6050_REG_ACCEL_CONFIG    0x1C
#define MPU6050_REG_ACCEL_XOUT_H    0x3B    /* 从这开始连读 14 字节 */
#define MPU6050_REG_TEMP_OUT_H      0x41
#define MPU6050_REG_GYRO_XOUT_H     0x43
#define MPU6050_REG_PWR_MGMT_1      0x6B
#define MPU6050_REG_WHO_AM_I        0x75

#define MPU6050_ACCEL_LSB_PER_G     16384   /* ±2g 量程 */

uint8_t MPU6050_Init(void);                                     /* 返回 0 = 成功 */
uint8_t MPU6050_ReadID(void);                                   /* 正常应返回 0x68 */

/* 一次连读 14 字节（加速度 6 + 温度 2 + 角速度 6），保证三轴是同一时刻的 */
uint8_t MPU6050_ReadAll(int16_t *acc, int16_t *gyro, int16_t *temp_raw);

/* 只读加速度，够水平仪用了 */
uint8_t MPU6050_ReadAccel(int16_t *ax, int16_t *ay, int16_t *az);

/* 温度原始值换算成摄氏度（整数） */
int16_t MPU6050_TempToC(int16_t temp_raw);

#endif
