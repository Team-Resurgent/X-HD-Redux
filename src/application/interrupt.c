#include "stm32f0xx_hal.h"
#include "../shared/adv7511.h"
#include "../shared/debug.h"
#include "smbus_i2c.h"

void SysTick_Handler(void) {
    HAL_IncTick();
    HAL_SYSTICK_IRQHandler();
}

void HardFault_Handler(void) {
    debug_log("HardFault occurred!\r\n");
    while (1);
}

extern adv7511 encoder;

void ADV_IRQ_HANDLER(void) {
    // Gated on EXTI->PR bit 7 specifically -- this shared EXTI4_15 vector also handles
    // lines 10/11 for the SMBus bus probe below.
    if (EXTI->PR & (1u << 7)) {
        encoder.interrupt = 1;
        HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_7);
    }
    if (EXTI->PR & (1u << 10)) {
        EXTI->PR = (1u << 10);
        smbus_probe_scl_edge();
    }
    if (EXTI->PR & (1u << 11)) {
        EXTI->PR = (1u << 11);
        smbus_probe_sda_edge();
    }
}
