// SPDX-License-Identifier: GPL-3.0-only
#include "gpio.h"
#include "stm32f0xx_hal.h"

#ifndef XHD_HOST_TEST
// The HAL encoding gpio_init() decodes, and the host-test model of the GPIO block relies on:
// [1:0] the MODER value, [4] open drain.
_Static_assert(GPIO_MODE_INPUT == 0x00 && GPIO_MODE_OUTPUT_PP == 0x01 && GPIO_MODE_OUTPUT_OD == 0x11 &&
               GPIO_MODE_AF_PP == 0x02 && GPIO_MODE_AF_OD == 0x12 && GPIO_MODE_ANALOG == 0x03 &&
               GPIO_NOPULL == 0 && GPIO_PULLUP == 1 && GPIO_PULLDOWN == 2 && GPIO_SPEED_FREQ_HIGH == 3,
               "HAL GPIO mode encoding");
#endif

void gpio_init(void *gpio_port, uint16_t pins, uint32_t mode, uint32_t pull, uint32_t speed, uint32_t af) {
    GPIO_TypeDef *const port = gpio_port;
    const uint32_t moder = mode & 3u;   // GPIO_MODE: the MODER value
    for (uint32_t pos = 0; pins >> pos; pos++) {
        if (!(pins & (1u << pos))) continue;
        if (moder == 1u || moder == 2u) {   // output, AF
            port->OSPEEDR = (port->OSPEEDR & ~(3u << (pos * 2u))) | (speed << (pos * 2u));
            port->OTYPER = (port->OTYPER & ~(1u << pos)) | (((mode >> 4) & 1u) << pos);
        }
        if (moder != 3u) {   // not analog
            port->PUPDR = (port->PUPDR & ~(3u << (pos * 2u))) | (pull << (pos * 2u));
        }
        if (moder == 2u) {
            port->AFR[pos >> 3] = (port->AFR[pos >> 3] & ~(0xFu << ((pos & 7u) * 4u))) | (af << ((pos & 7u) * 4u));
        }
        port->MODER = (port->MODER & ~(3u << (pos * 2u))) | (moder << (pos * 2u));
    }
}

void init_gpio() {
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    // Leds
    gpio_init(GPIOC, GPIO_PIN_13 | GPIO_PIN_14, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL, GPIO_SPEED_FREQ_LOW, 0);

    // Recovery jumper
    gpio_init(GPIOC, GPIO_PIN_15, GPIO_MODE_INPUT, GPIO_PULLUP, 0, 0);

    // ADV7511 Hardware Interrupt (PF7 EXTI, both edges): a plain input, then the EXTI line as
    // HAL_GPIO_Init(GPIO_MODE_IT_RISING_FALLING) set it -- port F on line 7, rising and
    // falling, no event, interrupt unmasked, in that order.
    gpio_init(GPIOF, GPIO_PIN_7, GPIO_MODE_INPUT, GPIO_NOPULL, 0, 0);
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    SYSCFG->EXTICR[1] = (SYSCFG->EXTICR[1] & ~(0xFu << 12)) | (5u << 12);
    EXTI->RTSR |= GPIO_PIN_7;
    EXTI->FTSR |= GPIO_PIN_7;
    EXTI->EMR &= ~(uint32_t)GPIO_PIN_7;
    EXTI->IMR |= GPIO_PIN_7;
}

void set_led_1(bool state) {
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, state ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void set_led_2(bool state) {
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_14, state ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

bool recovery_jumper_enabled() {
    return !HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_15);
}
