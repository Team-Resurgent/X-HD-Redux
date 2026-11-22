// SPDX-License-Identifier: GPL-3.0-only
#ifndef __GPIO_H__
#define __GPIO_H__

#include "stdbool.h"
#include <stdint.h>

// HAL_GPIO_Init() for the pins of `pins` on `port`, without the struct and without its EXTI
// part: per pin, in its order, OSPEEDR and OTYPER (output and AF modes), PUPDR (not analog),
// AFR (AF mode), then MODER. `mode` is a GPIO_MODE_INPUT / _OUTPUT_PP / _AF_PP / _AF_OD constant,
// `pull` GPIO_NOPULL / _PULLUP, `speed` a GPIO_SPEED_FREQ_* value, `af` the alternate
// function (AF modes only). `port` is a GPIO_TypeDef * (GPIOA...); void here so this header
// needs no HAL.
void gpio_init(void *port, uint16_t pins, uint32_t mode, uint32_t pull, uint32_t speed, uint32_t af);

void init_gpio();

void set_led_1(bool state); // Green led for the HD+
void set_led_2(bool state); // Blue led for the HD+
bool recovery_jumper_enabled();

#endif // __GPIO_H__
