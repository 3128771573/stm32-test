#include "i2c.h"

#define SCL_HIGH()      GPIO_SetBits(I2C_PORT, I2C_SCL_PIN)
#define SCL_LOW()       GPIO_ResetBits(I2C_PORT, I2C_SCL_PIN)
#define SDA_HIGH()      GPIO_SetBits(I2C_PORT, I2C_SDA_PIN)
#define SDA_LOW()       GPIO_ResetBits(I2C_PORT, I2C_SDA_PIN)
#define SDA_READ()      GPIO_ReadInputDataBit(I2C_PORT, I2C_SDA_PIN)

/* 半周期延时。用 volatile 计数器，防止编译器把空循环优化掉 */
static void i2c_delay(void)
{
	volatile uint8_t i = I2C_DELAY_LOOPS;
	while (i--)
	{
		/* 空转 */
	}
}

void SoftI2C_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

	GPIO_InitStructure.GPIO_Pin   = I2C_SCL_PIN | I2C_SDA_PIN;
	GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_OD;   /* 开漏，必须 */
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(I2C_PORT, &GPIO_InitStructure);

	SCL_HIGH();
	SDA_HIGH();
}

/*---------------------------------------------------------------------------
  总线恢复：如果上一次通信被中途打断（比如复位时从机正数到一半），
  从机会一直把 SDA 拉低，之后的通信就全废了。
  标准解法是补 9 个时钟让从机把没发完的字节走完，再补一个 STOP。
  上电时先调一次，能省掉很多"明明接对了却扫不到器件"的怪问题。
---------------------------------------------------------------------------*/
void SoftI2C_BusRecover(void)
{
	uint8_t i;

	SDA_HIGH();
	SCL_HIGH();
	i2c_delay();

	if (SDA_READ() == 0)                /* SDA 被拉死了才需要补时钟 */
	{
		for (i = 0; i < 9; i++)
		{
			SCL_LOW();
			i2c_delay();
			SCL_HIGH();
			i2c_delay();
		}
	}

	/* 补一个停止条件，把总线交回空闲态 */
	SDA_LOW();
	i2c_delay();
	SCL_HIGH();
	i2c_delay();
	SDA_HIGH();
	i2c_delay();
}

static void i2c_start(void)
{
	SDA_HIGH();
	i2c_delay();
	SCL_HIGH();
	i2c_delay();
	SDA_LOW();          /* SCL 为高时 SDA 下降沿 = 起始 */
	i2c_delay();
	SCL_LOW();
	i2c_delay();
}

static void i2c_stop(void)
{
	SDA_LOW();
	i2c_delay();
	SCL_HIGH();
	i2c_delay();
	SDA_HIGH();         /* SCL 为高时 SDA 上升沿 = 停止 */
	i2c_delay();
}

/* 返回 0 = 从机应答，1 = 无应答 */
static uint8_t i2c_send_byte(uint8_t byte)
{
	uint8_t i, ack;

	for (i = 0; i < 8; i++)             /* 高位先发 */
	{
		if (byte & 0x80) SDA_HIGH(); else SDA_LOW();
		i2c_delay();
		SCL_HIGH();
		i2c_delay();
		SCL_LOW();
		i2c_delay();
		byte <<= 1;
	}

	/* 第 9 个时钟：放开 SDA，读从机的应答 */
	SDA_HIGH();
	i2c_delay();
	SCL_HIGH();
	i2c_delay();
	ack = (SDA_READ() == 0) ? 0 : 1;
	SCL_LOW();
	i2c_delay();

	return ack;
}

/* send_ack: 1 = 读完发应答（还要继续读），0 = 发非应答（最后一个字节） */
static uint8_t i2c_read_byte(uint8_t send_ack)
{
	uint8_t i, byte = 0;

	SDA_HIGH();                         /* 主机释放 SDA，转为输入 */

	for (i = 0; i < 8; i++)
	{
		byte <<= 1;
		SCL_HIGH();
		i2c_delay();
		if (SDA_READ()) byte |= 0x01;   /* SCL 高电平期间采样 */
		SCL_LOW();
		i2c_delay();
	}

	if (send_ack) SDA_LOW(); else SDA_HIGH();
	i2c_delay();
	SCL_HIGH();
	i2c_delay();
	SCL_LOW();
	i2c_delay();
	SDA_HIGH();

	return byte;
}

/* 探测某个 7 位地址上有没有器件，返回 1 = 在 */
uint8_t SoftI2C_Probe(uint8_t addr7)
{
	uint8_t ack;

	i2c_start();
	ack = i2c_send_byte((uint8_t)(addr7 << 1));
	i2c_stop();

	return (ack == 0) ? 1 : 0;
}

uint8_t SoftI2C_WriteBytes(uint8_t addr7, const uint8_t *buf, uint16_t len)
{
	uint16_t i;

	i2c_start();
	if (i2c_send_byte((uint8_t)(addr7 << 1)) != 0)
	{
		i2c_stop();
		return 1;
	}

	for (i = 0; i < len; i++)
	{
		if (i2c_send_byte(buf[i]) != 0)
		{
			i2c_stop();
			return 1;
		}
	}

	i2c_stop();
	return 0;
}

uint8_t SoftI2C_ReadBytes(uint8_t addr7, uint8_t *buf, uint16_t len)
{
	uint16_t i;

	i2c_start();
	if (i2c_send_byte((uint8_t)((addr7 << 1) | 0x01)) != 0)
	{
		i2c_stop();
		return 1;
	}

	for (i = 0; i < len; i++)
	{
		buf[i] = i2c_read_byte((uint8_t)((i < len - 1) ? 1 : 0));
	}

	i2c_stop();
	return 0;
}

uint8_t SoftI2C_WriteReg(uint8_t addr7, uint8_t reg, uint8_t val)
{
	uint8_t buf[2];

	buf[0] = reg;
	buf[1] = val;

	return SoftI2C_WriteBytes(addr7, buf, 2);
}

/* 先写寄存器地址，再用重复起始条件转读 */
uint8_t SoftI2C_ReadRegs(uint8_t addr7, uint8_t reg, uint8_t *buf, uint16_t len)
{
	uint16_t i;

	i2c_start();
	if (i2c_send_byte((uint8_t)(addr7 << 1)) != 0)
	{
		i2c_stop();
		return 1;
	}
	if (i2c_send_byte(reg) != 0)
	{
		i2c_stop();
		return 1;
	}

	i2c_start();                        /* 重复起始条件 */
	if (i2c_send_byte((uint8_t)((addr7 << 1) | 0x01)) != 0)
	{
		i2c_stop();
		return 1;
	}

	for (i = 0; i < len; i++)
	{
		buf[i] = i2c_read_byte((uint8_t)((i < len - 1) ? 1 : 0));
	}

	i2c_stop();
	return 0;
}

uint8_t SoftI2C_ReadReg(uint8_t addr7, uint8_t reg, uint8_t *val)
{
	return SoftI2C_ReadRegs(addr7, reg, val, 1);
}

/*---------------------------------------------------------------------------
  流式写接口：把底层的起始/发送/停止暴露出来，
  这样调用者可以在一个事务里连着发几千个字节（OLED 整屏刷新就靠它）。
---------------------------------------------------------------------------*/
void SoftI2C_Begin(uint8_t addr7)
{
	i2c_start();
	i2c_send_byte((uint8_t)(addr7 << 1));
}

uint8_t SoftI2C_SendByte(uint8_t val)
{
	return i2c_send_byte(val);
}

void SoftI2C_End(void)
{
	i2c_stop();
}
