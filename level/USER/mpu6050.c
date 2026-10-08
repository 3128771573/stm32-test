#include "mpu6050.h"
#include "i2c.h"
#include "delay.h"

uint8_t MPU6050_ReadID(void)
{
	uint8_t id = 0;

	if (SoftI2C_ReadReg(MPU6050_ADDR7, MPU6050_REG_WHO_AM_I, &id) != 0)
	{
		return 0xFF;                    /* 读失败 */
	}
	return id;
}

uint8_t MPU6050_Init(void)
{
	/* 先复位，让芯片回到确定的初始状态 */
	SoftI2C_WriteReg(MPU6050_ADDR7, MPU6050_REG_PWR_MGMT_1, 0x80);
	delay_ms(100);

	/* 解除休眠，时钟源选 X 轴陀螺的 PLL（比内部 8MHz 稳） */
	SoftI2C_WriteReg(MPU6050_ADDR7, MPU6050_REG_PWR_MGMT_1, 0x01);
	delay_ms(10);

	/* 采样率：陀螺输出 1kHz，分频 4 -> 200Hz。
	   调高到 200Hz 是为了让软件能采得密一点（主循环每 5ms 读一次），
	   这样刷屏能做快，而不会牺牲滤波效果。 */
	SoftI2C_WriteReg(MPU6050_ADDR7, MPU6050_REG_SMPLRT_DIV, 0x04);

	/* 数字低通 44Hz：水平仪要的是稳，不是快 */
	SoftI2C_WriteReg(MPU6050_ADDR7, MPU6050_REG_CONFIG, 0x03);

	/* 陀螺 ±250 度/秒，不自检 */
	SoftI2C_WriteReg(MPU6050_ADDR7, MPU6050_REG_GYRO_CONFIG, 0x00);

	/* 加速度 ±2g，不自检。
	   注意这里必须是 0x00：原厂 51 例程给的是 0x01，
	   那个值会打开加速度的高通滤波，把重力分量滤掉，算倾角就废了。 */
	SoftI2C_WriteReg(MPU6050_ADDR7, MPU6050_REG_ACCEL_CONFIG, 0x00);

	delay_ms(10);

	if (MPU6050_ReadID() != MPU6050_ADDR7) return 1;

	return 0;
}

uint8_t MPU6050_ReadAll(int16_t *acc, int16_t *gyro, int16_t *temp_raw)
{
	uint8_t buf[14];

	if (SoftI2C_ReadRegs(MPU6050_ADDR7, MPU6050_REG_ACCEL_XOUT_H, buf, 14) != 0)
	{
		return 1;
	}

	acc[0] = (int16_t)((buf[0] << 8) | buf[1]);     /* AX */
	acc[1] = (int16_t)((buf[2] << 8) | buf[3]);     /* AY */
	acc[2] = (int16_t)((buf[4] << 8) | buf[5]);     /* AZ */
	*temp_raw = (int16_t)((buf[6] << 8) | buf[7]);  /* 温度 */
	gyro[0] = (int16_t)((buf[8] << 8) | buf[9]);    /* GX */
	gyro[1] = (int16_t)((buf[10] << 8) | buf[11]);  /* GY */
	gyro[2] = (int16_t)((buf[12] << 8) | buf[13]);  /* GZ */

	return 0;
}

uint8_t MPU6050_ReadAccel(int16_t *ax, int16_t *ay, int16_t *az)
{
	uint8_t buf[6];

	if (SoftI2C_ReadRegs(MPU6050_ADDR7, MPU6050_REG_ACCEL_XOUT_H, buf, 6) != 0)
	{
		return 1;
	}

	*ax = (int16_t)((buf[0] << 8) | buf[1]);
	*ay = (int16_t)((buf[2] << 8) | buf[3]);
	*az = (int16_t)((buf[4] << 8) | buf[5]);

	return 0;
}

/* 数据手册给的换算：TEMP = TEMP_OUT / 340 + 36.53
   这里只用整数运算，避免把浮点库拉进工程 */
int16_t MPU6050_TempToC(int16_t temp_raw)
{
	return (int16_t)(temp_raw / 340 + 36);
}
