#include "stm32f0xx_hal.h"
#include "stm32f0xx.h"
#include "../error_handler.h"

void SystemClock_Config(void)
{
    // MCU runs HSI/2 -> PLL x6 = 24 MHz rather than raw HSI (8 MHz): the passive SMBus
    // probe's ISR needs the headroom against a real I2C bit's ~5 us budget. On the F030x8
    // the PLL input is fixed at HSI/2 (4 MHz), so x6 is 24 MHz, not the 48 MHz this was
    // written for (measured: TIM1 /4 gave 6 MHz). SystemCoreClockUpdate() computes the real
    // value, so HAL-derived rates (UART BRR, SysTick) are right; the I2C2 TIMINGR and the
    // busy-wait counts were sized for 48 MHz and run 2x slow (each site says so).
    // debug_init() must run before this function (see main()'s comment): if this hits
    // _Error_Handler() (which itself calls debug_log()), a failure here before debug_init()
    // has run would hang silently instead of printing.
    // I2C1 (ADV7511, below) deliberately stays on raw HSI, untouched by this. Everything
    // else that's clock-derived (I2C2 Timing, a couple of MCU-cycle busy-wait loops; USART2
    // BRR is computed from the clock, see debug.c) is called out at each site.
    //
    // Direct register sequence instead of HAL_RCC_OscConfig()/HAL_RCC_ClockConfig()/
    // HAL_RCCEx_PeriphCLKConfig() -- this MCU only ever uses that generic every-
    // oscillator/every-variant HAL abstraction to set one fixed configuration, once, at
    // boot, and nothing else in the codebase calls any of the three, so --gc-sections
    // can drop all three implementations entirely. Same final register values as the HAL
    // calls (RCC_CR/RCC_CFGR/RCC_CFGR2/RCC_CFGR3 bit macros are identical either way, and
    // this is the same sequence those HAL functions perform internally), same
    // _Error_Handler() escape if the hardware fails to lock/switch.

    // 1. HSI is already enabled+ready out of reset (POR default), but confirm rather than
    // assume -- cheap, and this is the one clock source everything else here depends on.
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) { }

    // 2. PLL config bits (CFGR.PLLSRC/PLLMUL, CFGR2.PREDIV) are only writable while the PLL
    // itself is disabled (RM0360) -- off by POR default at this point, but this function
    // only ever runs once per boot so this is defensive, not load-bearing.
    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY) { }

    // 3. Same PLL source/multiplier the HAL struct fields this replaces used --
    // PLLSource = RCC_PLLSOURCE_HSI, PLLMUL = RCC_PLL_MUL6 -- both resolve to the exact same
    // RCC_CFGR bit patterns here as they did through HAL_RCC_OscConfig(), so this reproduces
    // the same 24 MHz this codebase has run at throughout, not a fresh derivation.
    // (RCC_PLLSOURCE_HSI's own value is chip-variant-dependent -- plain
    // STM32F030x8 like this one only has a 1-bit PLLSRC mux, fixed HSI/2 into the PLL, no
    // PREDIV option for the HSI path at all, unlike F070xB/F030xC's wider mux -- but since
    // both the old and new code reference the identical macro, this is unaffected either
    // way.) PREDIV only affects the HSE path on this variant, not HSI -- set to DIV1 to
    // match the HAL struct's own PREDIV field regardless, since that's its reset default too.
    MODIFY_REG(RCC->CFGR2, RCC_CFGR2_PREDIV, RCC_PREDIV_DIV1);
    MODIFY_REG(RCC->CFGR, RCC_CFGR_PLLMUL | RCC_CFGR_PLLSRC, RCC_PLL_MUL6 | RCC_PLLSOURCE_HSI);

    // 4. Enable PLL and wait for lock -- bounded so a genuine hardware failure to lock
    // still reaches _Error_Handler() instead of hanging forever (same contract
    // HAL_RCC_OscConfig() itself provides via its own internal timeout).
    RCC->CR |= RCC_CR_PLLON;
    uint32_t pll_timeout = 20000;
    while (!(RCC->CR & RCC_CR_PLLRDY) && --pll_timeout) { }
    if (!pll_timeout)
    {
        Error_Handler();
    }

    // 5. FLASH_LATENCY_1 (1 wait state). RM0360 requires it only above 24 MHz, so at
    // exactly 24 MHz it isn't needed; kept, since raising the PLL would need it.
    // Set BEFORE switching SYSCLK to the (faster) PLL output, same order HAL_RCC_
    // ClockConfig() itself enforces internally.
    __HAL_FLASH_SET_LATENCY(FLASH_LATENCY_1);
    while (__HAL_FLASH_GET_LATENCY() != FLASH_LATENCY_1) { }

    // 6. AHB (HCLK) and APB1 (PCLK1) prescalers: both DIV1, same as the HAL struct's
    // AHBCLKDivider/APB1CLKDivider fields this replaces.
    MODIFY_REG(RCC->CFGR, RCC_CFGR_HPRE | RCC_CFGR_PPRE, RCC_SYSCLK_DIV1 | RCC_HCLK_DIV1);

    // 7. Switch SYSCLK to the now-locked PLL and wait for the switch to actually take
    // (SWS mirrors SW after a few clock cycles, not instantly) -- bounded the same way
    // step 4 is.
    MODIFY_REG(RCC->CFGR, RCC_CFGR_SW, RCC_SYSCLKSOURCE_PLLCLK);
    uint32_t sws_timeout = 20000;
    while (((RCC->CFGR & RCC_CFGR_SWS) != RCC_SYSCLKSOURCE_STATUS_PLLCLK) && --sws_timeout) { }
    if (!sws_timeout)
    {
        Error_Handler();
    }

    SystemCoreClockUpdate();

    // 8. I2C1 (ADV7511) deliberately stays on raw HSI (8MHz), independent of the PLL/HCLK
    // switch above -- same RCC_I2C1CLKSOURCE_HSI the HAL struct's I2c1ClockSelection field
    // set.
    MODIFY_REG(RCC->CFGR3, RCC_CFGR3_I2C1SW, RCC_I2C1CLKSOURCE_HSI);

    // Configure the Systick interrupt time
    HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq() / 1000);

    // Configure the Systick
    HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);

    // SysTick_IRQn priority -- deliberately below (numerically higher than) the shared
    // EXTI4_15 vector (ADV7511 + passive SMBus bus probes, priority 2). EXTI->PR is a
    // sticky level flag rather than a counter, so a SysTick preemption landing mid-bit
    // could in theory let a second same-line edge get silently coalesced away before the
    // delayed probe ISR ever cleared/read it; keeping SysTick at lower priority avoids that.
    // HAL_GetTick() accuracy is unaffected: the SysTick counter is free-running in hardware
    // regardless of ISR-service latency, and nothing here starves it anywhere near the 2ms
    // (two full periods) it'd take to actually lose a tick.
    HAL_NVIC_SetPriority(SysTick_IRQn, 3, 0);
}
