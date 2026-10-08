#ifndef __STM32F10x_CONF_H
#define __STM32F10x_CONF_H

/*---------------------------------------------------------------------------
  标准外设库的模块开关：工程里 include 哪些外设头文件。
  本工程只用软件 I2C（不占用硬件 I2C 外设），所以只需要下面三个。
  以后要加串口打印，把 usart 那行放开即可。
---------------------------------------------------------------------------*/
#include "stm32f10x_rcc.h"
#include "stm32f10x_gpio.h"
#include "misc.h"

/* #include "stm32f10x_usart.h" */

#ifdef  USE_FULL_ASSERT
  void assert_failed(uint8_t* file, uint32_t line);
  #define assert_param(expr) ((expr) ? (void)0 : assert_failed((uint8_t *)__FILE__, __LINE__))
#else
  #define assert_param(expr) ((void)0)
#endif

#endif
