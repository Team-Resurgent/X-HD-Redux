#include "main.h"
#include "stm32f0xx_hal.h"
#include "stm32f0xx.h"
#include <string.h>
#include "../shared/adv7511_xbox.h"
#include "../shared/crc32.h"
#include "../shared/defines.h"
#include "../shared/debug.h"
#include "../shared/error_handler.h"
#include "../shared/gpio.h"
#include "../shared/user_config.h"
#include "../shared/flash.h"
#include "bl_video.h"
#include "bl_uart_update.h"
#include "smbus_i2c.h"

extern void SystemClock_Config(void);

adv7511 encoder;

// Genuinely "not yet determined" (see types.h) rather than an arbitrary real encoder --
// main() below overrides it with the persisted value on any board that's already been
// through a real application boot, so this only matters for a truly first-ever boot
// landing here before the application has ever run once, and picking a real encoder here
// would unfairly skip that board's self-latch write path for whichever one got picked.
xbox_encoder xb_encoder = ENCODER_UNKNOWN;

int main(void)
{
    HAL_Init();

    // debug_init() called before AND after SystemClock_Config() -- see application/main.c's
    // matching comment for why: makes a SystemClock_Config() failure visible
    // over UART instead of a silent _Error_Handler() hang, then finalizes BRR for whatever
    // clock it actually lands on. debug_init() is idempotent, safe to call twice.
    debug_init();
    SystemClock_Config();
    debug_init();
    init_gpio();

    debug_log("Entering Bootloader...\r\n");

    // Flash-only read, safe this early -- see the same pattern in application/main.c.
    xbox_encoder persisted_encoder;
    if (user_config_peek_active_encoder(&persisted_encoder)) {
        xb_encoder = persisted_encoder;
    }

    uint32_t flag_value = *BOOTLOADER_FLAG_ADDRESS;
    bool magic_set = (flag_value == BOOTLOADER_MAGIC_VALUE);
    bool recovery = recovery_jumper_enabled();
    *BOOTLOADER_FLAG_ADDRESS = 0;

    // RECOVERY jumper (PC15 / pin 4, on the hw revisions that route it to the "RECOVERY"
    // pad) held to ground: wipe the user config in Page 63 so the application's
    // user_config_init() falls back to factory defaults next boot -- and, since the page
    // then has no valid magic, the encoder probe re-detects from scratch (covers a board
    // moved to a console with a different video encoder). The use case is "the app boots
    // but its config / encoder self-latch is wrong", so this is gated on
    // can_launch_application(): Page 63 also holds the updater's app-invalid flag in its
    // last 2 bytes, and a full-page erase would clear it -- skipping the wipe while that
    // flag is set (mid firmware-recovery) leaves it intact, and a config wipe is moot then
    // anyway. Also gated on a valid config actually being present, so leaving the jumper
    // installed across power cycles is a no-op, not an erase every boot. Additive: a
    // grounded jumper still holds the board in FW-recovery mode via the launch guard below.
    if (recovery && can_launch_application() &&
        ((const xhd_user_config *)XHD_CONFIG_FLASH_ADDR)->magic == XHD_CONFIG_MAGIC) {
        debug_log("[RECOVERY] jumper grounded -- erasing user config (Page 63)\r\n");
        flash_erase_page(XHD_CONFIG_FLASH_PAGE);
    }

    if (!magic_set && !recovery && can_launch_application()) {
        jump_to_application();
    }

    // If magic is set, go straight to FW update mode and dont setup the adv
    if (magic_set) {
        enter_bootloader_mode_fw();
    } else {
        enter_bootloader_mode();
    }
}

bool can_launch_application(void)
{
    volatile uint32_t *app_vector_table = (volatile uint32_t *)APP_START_ADDRESS;
    uint32_t app_entry = app_vector_table[1];

    // Reject any reset-vector entry point outside the application's own flash region --
    // guards against jumping into a corrupted image using a stray/garbage entry point.
    if (app_entry < APP_START_ADDRESS || app_entry >= (FLASH_START_ADDRESS + FLASH_TOTAL_SIZE)) {
        return false;
    }

    if (app_entry == 0xFFFFFFFF) {
        return false;
    }

    // Cortex-M0 branches only to Thumb addresses (bit 0 set) -- an even entry point
    // (e.g. from a flash write interrupted mid-word) would hard-fault instead of
    // falling back to recovery here, where the other checks above are meant to catch it.
    if ((app_entry & 1) == 0) {
        return false;
    }

    uint16_t flash_flag = *(volatile uint16_t *)APP_INVALID_FLAG_ADDRESS;
    if (flash_flag == APP_INVALID_FLAG) {
        return false;
    }

    return true;
}

void jump_to_application(void)
{
    debug_log("Launching Application...\r\n");

    volatile uint32_t *app_vector_table = (volatile uint32_t *)APP_START_ADDRESS;
    uint32_t app_entry = app_vector_table[1];

    __disable_irq();

    // Leave nothing armed for the application to inherit. SysTick keeps running and
    // TICKINT stays set across this jump otherwise -- if it fires between here and the
    // application's own HAL_Init()/SysTick reconfiguration re-enabling interrupts, that
    // exception latches pending and immediately vectors into the application's SysTick
    // handler the moment it calls __enable_irq(), before its own tick state is ready for
    // it. Any bootloader-enabled NVIC channel (ADV7511 EXTI, I2C2) would carry over the
    // same way. Cheap and safe regardless of whether this has ever actually been observed
    // to matter in practice.
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;
    SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk;
    NVIC->ICER[0] = 0xFFFFFFFF;
    NVIC->ICPR[0] = 0xFFFFFFFF;

    void (*app_reset_handler)(void) = (void (*)(void))app_entry;
    __set_MSP(app_vector_table[0]);
    app_reset_handler();

    while (1);
}

void enter_bootloader_mode(void)
{
    debug_log("Waiting for fw recovery...\r\n");

    // ADV_IRQ_HANDLER (bootloader/interrupt.c) only runs on real ADV7511
    // interrupt activity (HPD/MSEN change) if the NVIC IRQ is enabled -- without
    // it the EXTI peripheral just latches the pending edge and the CPU never
    // vectors into the handler, so adv_handle_interrupts() below would never see
    // a real interrupt and this mode would never get hotplug-triggered
    // recalibration. Enabled only here, not in main() before the mode branch --
    // enter_bootloader_mode_fw() must leave the ADV chip and its interrupt state
    // completely untouched. Priority 2, below I2C2 (1), as in the application (its main.c
    // priority map): the console's SMBus must never wait on an ADV7511 interrupt.
    HAL_NVIC_SetPriority(ADV_IRQ_CHANNEL, 2, 0);
    HAL_NVIC_EnableIRQ(ADV_IRQ_CHANNEL);

    // Before any ADV7511 config, same reasoning as application/main.c's identical fix:
    // this can be reached with a console already mid-boot (RECOVERY jumper, or a bad app
    // image) that's still polling SMBus, and adv7511_reinit_chip() below can wait
    // indefinitely on the display's HPD/MSEN -- that must never gate the Xbox's own bus.
    smbus_i2c_init();

    static uint32_t last_blink = 0;
    static bool led_state = false;
    // True once adv7511_reinit_chip() has run -- see the wait below. Gates the
    // display-dependent calls (interrupt handling, TMDS cycle service, video-follow), none
    // of which are meaningful before the chip has a baseline configuration.
    bool chip_configured = false;

    while(1) {
        if ((HAL_GetTick() - last_blink) > 100) {
            led_state = !led_state;
            set_led_1(led_state);
            set_led_2(!led_state);
            last_blink = HAL_GetTick();
        }
        HAL_Delay(10);

        if (!chip_configured) {
            // No debounce beyond this loop's own 10ms cadence (unlike video_supervisor.c's
            // explicit one) -- this is a manual, human-initiated recovery session rather than
            // unattended runtime, so a rare early configure costs nothing. Requires HPD AND Monitor Sense together (ADV7511 programming guide
            // 4.2.1), with no timeout: every register adv7511_reinit_chip() would write is
            // a no-op while HPD is low, so there's nothing to gain by giving up and calling
            // it anyway.
            uint8_t reg42 = adv7511_read_register(0x42);
            if (((reg42 >> 6) & 0x01) && ((reg42 >> 5) & 0x01)) {
                adv7511_reinit_chip(&encoder, xb_encoder);
                // adv7511_reinit_chip() opens the input gate itself and powers the TMDS
                // output channels DOWN at the end of config (so the application's PLL can
                // lock on a clean open reference). The bootloader's standalone
                // passthrough has no mode-apply path to bring the output back up, so do
                // it here. This mode is
                // a deliberate, rare excursion (waiting for a firmware update), so there's
                // no concern about briefly driving a stale signal to the sink -- just
                // un-gate and power up immediately.
                adv7511_enable_video();
                adv7511_power_up_tmds();
                chip_configured = true;
            }
        } else {
            // ADV handling for VIC mode for emergency
            adv_handle_interrupts(&encoder);

            // Advance the non-blocking TMDS cycle bl_video_follow() drives on a mode apply.
            adv7511_tmds_cycle_service();

            // Minimal cold-start video: follow the 0x3E line detector, blind -> VIC 1.
            // Replaces the full stand_alone_loop() (+ its per-encoder timing tables and
            // family model) -- freed for the UART firmware updater below.
            bl_video_follow();
        }

        // Firmware update over UART (custom framed protocol) -- also serviced here so a
        // board that fell through to this "app invalid / RECOVERY jumper" path can still
        // be recovered from a PC with only a serial adapter.
        bl_uart_update_poll();
    }
}

void enter_bootloader_mode_fw(void)
{
    debug_log("Waiting for update...\r\n");

    smbus_i2c_init();

    set_led_1(false);
    set_led_2(true);

    while(1) {
        HAL_Delay(10);
        // SMBus flashing (XHDUpdater.xbe) is serviced entirely from the I2C2 slave
        // ISR; this loop only needs to poll the UART update path.
        bl_uart_update_poll();
    }
}
