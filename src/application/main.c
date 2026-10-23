// Copyright 2021, Ryan Wendland, XboxHDMI by Ryzee119
// SPDX-License-Identifier: MIT

#include "stm32f0xx_hal.h"
#include "../shared/adv7511_i2c.h"
#include "../shared/adv7511.h"
#include "../shared/adv7511_xbox.h"
#include "../shared/debug.h"
#include "../shared/xbox_video_standalone.h"
#include "../shared/error_handler.h"
#include "../shared/gpio.h"
#include "../shared/defines.h"
#include "smbus_i2c.h"
#include "xbox_video_bios.h"
#include "video_supervisor.h"

#include "../shared/adv7511_edid.h"
#include "../shared/user_config.h"

adv7511 encoder;

extern void SystemClock_Config(void);

#define VECTOR_TABLE_SIZE 48  // Covers 0xC0 bytes (16 + IRQs)
#define APP_VECTOR_TABLE  ((uint32_t*)APP_START_ADDRESS)
#define RAM_VECTOR_TABLE  ((uint32_t*)RAM_START_ADDRESS)

// CEC_CLK from the MCU: PA8 (pin 29) is wired to ADV7511 pin 50 on some board revisions, and
// can be hand-wired on a board whose 12 MHz CEC oscillator has failed. TIM1 CH1 PWM at
// SYSCLK / 12 MHz, 50% duty -- the ADV7511's CEC defaults assume 12 MHz, so nothing changes
// there. The divider comes from SystemCoreClock (24 MHz, so 2). TIM1 runs at SYSCLK (APB
// prescaler 1). A page-63 switch, on by default.
// Starts the clock when the setting turns on, from any path (CLI, SMBus 156, load); turning
// it off takes effect at the next boot (PA8 stays an input from reset). The CEC block is
// re-initialised one pass after the clock starts: its registers only take writes with the
// clock running, and an init in the same breath as the clock start once left a send stuck.
void cec_clock_out_follow_config(void)
{
    static uint8_t s_state = 0;   // 0 off, 1 clock started (init next pass), 2 running
    if (s_state == 1) {
        s_state = 2;
        adv7511_cec_init();
    }
    if (s_state == 0 && !user_config_get()->cec_clock_off) {
        s_state = 1;
        RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
        const uint32_t div = SystemCoreClock / 12000000u;   // 2 at 24 MHz
        TIM1->ARR = div - 1u;
        TIM1->CCR1 = div >> 1;
        TIM1->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1;   // PWM mode 1
        TIM1->CCER = TIM_CCER_CC1E;
        TIM1->BDTR = TIM_BDTR_MOE;                           // TIM1 is an advanced timer
        TIM1->CR1 = TIM_CR1_CEN;
        GPIOA->OSPEEDR |= 3u << 16;                          // PA8 high speed
        MODIFY_REG(GPIOA->AFR[1], 0xFu, 2u);                 // PA8 AF2 = TIM1_CH1
        MODIFY_REG(GPIOA->MODER, 3u << 16, 2u << 16);        // alternate function
    }
}

void relocate_vector_table_to_ram(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    for (uint32_t i = 0; i < VECTOR_TABLE_SIZE; i++) {
        RAM_VECTOR_TABLE[i] = APP_VECTOR_TABLE[i];
    }
    __HAL_SYSCFG_REMAPMEMORY_SRAM();
}


int main(void)
{
    // Best-known encoder for this console: the one saved on page 63 (a flash-only read, no
    // I2C, safe before init_adv()), else ENCODER_UNKNOWN until the probe or a BIOS packet
    // names it.
    xbox_encoder xb_encoder;
    if (!user_config_peek_active_encoder(&xb_encoder)) {
        xb_encoder = ENCODER_UNKNOWN;
    }

    // Set by bios_loop()/smbus_check_probed_encoder() when real evidence corrects the
    // initial encoder guess.
    bool encoder_mismatch = false;

    relocate_vector_table_to_ram();

    HAL_Init();

    // ADV7511 programming guide 4.1: wait 200ms for the address to be decided after power
    // supplies are high, before any I2C access. HAL_GetTick() only becomes valid after
    // HAL_Init(), so it's used as the reference point. The wait is consumed later, just
    // before the first real ADV7511 I2C access (debug_print_full_dump()), not blocked on
    // here -- everything in between takes real time and never touches the ADV7511.
    uint32_t adv_ready_not_before_tick = HAL_GetTick();

    // Called here, before SystemClock_Config(), so a clock-config failure
    // (_Error_Handler(), which calls debug_log()) is visible over UART instead of a
    // silent hang -- HSI is already running and this call's BRR matches it. Called again
    // below once the final clock is set, to recompute BRR for it; idempotent.
    debug_init();
    SystemClock_Config();
    debug_init();
    __enable_irq();

    init_gpio();

    // Interrupt priorities, the one place they are explained:
    //   1  I2C2 (the console's SMBus, smbus_slave_init()) -- the console's traffic first
    //   2  EXTI4_15: the ADV7511 INT pin (PF7) and the passive SMBus probe (PB10/PB11)
    //   3  USART2 (debug_init()) and SysTick
    // I2C2 must outrank EXTI4_15: an ADV7511 interrupt burst (HPD/PLL churn) preempting the
    // slave ISR mid-ACK left I2C2 holding SCL low for 20+ seconds (WRITE_CONFIG_BANK then
    // WRITE_CONFIG_INDEX) -- the ordering prevents it, so no stuck-SCL watchdog is needed.
    HAL_NVIC_SetPriority(ADV_IRQ_CHANNEL, 2, 0);
    HAL_NVIC_EnableIRQ(ADV_IRQ_CHANNEL);

    // Arm the passive SMBus edge probe NOW -- some BIOSes write the Conexant IN_MODE
    // pixel-format register only once, early in the console's boot. Only needs the
    // EXTI4_15 vector enabled just above (shared with ADV_IRQ_CHANNEL).
    smbus_probe_early_init();

    // The Xbox-facing SMBus slave (I2C2) comes up first: the console's SMC polls this bus and
    // can crash if it goes unanswered for ~100 ms. Self-contained -- its ISR only stashes data
    // for bios_loop(), and nothing here touches the ADV7511. An SMBus EDID read before the
    // chip is configured sees a zeroed buffer.
    smbus_i2c_init();

    // MCU-side only (I2C1 GPIO/peripheral) -- does not touch the ADV7511 chip itself. The
    // chip's own configuration (adv7511_reinit_chip()) is deferred to the non-blocking
    // HPD+MSEN wait in the main loop below.
    init_adv();
    user_config_init(xb_encoder);

    // Exact wording is a host-side contract: xhd_daemon.py's AUTO_TRIGGERS keys off this
    // substring to detect an MCU reboot -- don't change it without updating that too.
    // Printed unconditionally, before the chip is configured, so it's visible even with
    // no display connected.
    debug_log("\r\nX-HD ready\r\n");

    // Consume whatever's left of the 200ms ADV7511 power-up budget -- debug_print_full_dump()
    // below is the first real ADV7511 I2C access this boot.
    int32_t adv_wait_remaining_ms = (int32_t)(adv_ready_not_before_tick + 200 - HAL_GetTick());
    if (adv_wait_remaining_ms > 0) {
        HAL_Delay((uint32_t)adv_wait_remaining_ms);
    }
    debug_log("[ADV WAIT] %ldms\r\n", (long)(adv_wait_remaining_ms > 0 ? adv_wait_remaining_ms : 0));

    debug_print_full_dump();

    while (true)
    {
        video_supervisor_step(&encoder, &xb_encoder, &encoder_mismatch);
    }
}
