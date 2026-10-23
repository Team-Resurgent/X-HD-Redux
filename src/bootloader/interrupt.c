#include "stm32f0xx_hal.h"
#include "../shared/debug.h"
#include "../shared/defines.h"
#include "../shared/adv7511.h"

// Bootloader interrupt vector handlers. Vector table index = 16 + IRQn for
// external interrupts (indices 0-15 are the fixed Cortex-M0 exception slots):
// [0] = Initial Stack Pointer
// [1] = Reset Handler
// [2] = NMI Handler
// [3] = HardFault Handler
// ...
// [15] = SysTick Handler (exception #15)
// [23] = EXTI4_15_IRQHandler (IRQ #7) -- ADV_IRQ_HANDLER (shared/defines.h)
// [40] = I2C2_IRQHandler (IRQ #24) -- shared/smbus_slave_common.c

extern adv7511 encoder;

void SysTick_Handler(void)
{
    HAL_IncTick();
    HAL_SYSTICK_IRQHandler();
}

void HardFault_Handler(void)
{
    debug_log("HardFault occurred!\r\n");
    while (1);
}

void ADV_IRQ_HANDLER(void)
{
    // ADV7511 interrupt (HPD/MSEN change) on EXTI line 7. Must clear the pending bit via
    // HAL_GPIO_EXTI_IRQHandler() or the NVIC re-enters this handler immediately.
    if (EXTI->PR & (1u << 7)) {
        encoder.interrupt = 1;
        HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_7);
    }
}
