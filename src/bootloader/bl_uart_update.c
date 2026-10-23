#include "bl_uart_update.h"

#include "stm32.h"
#include "../shared/debug.h"
#include "../shared/defines.h"
#include "../shared/flash.h"
#include "../shared/crc32.h"
#include "../shared/gpio.h"

#define BL_UART_PROTO_VER   3
#define BL_UART_PAGE_SIZE   (1u << FLASH_PAGE_SHIFT)   // one flash page per frame
#define BL_PRE0             0xA5
#define BL_PRE1             0x5A

#define BL_FLAG_KEEP_P63    0x01   // hello flags bit 0: preserve page 63

// First writable app page / one-past-last: page 63 (user_config + the app-invalid flag) is
// never written by an image frame -- the flag and the config are handled at the end.
#define BL_FIRST_APP_PAGE   (BOOTLOADER_SIZE >> FLASH_PAGE_SHIFT)   /* 20 */
#define BL_END_APP_PAGE     LAST_FLASH_PAGE                         /* 63 */
#define BL_UART_MAX_IMG     ((BL_END_APP_PAGE - BL_FIRST_APP_PAGE) * BL_UART_PAGE_SIZE) /* 44032 */

// Rolling 4-byte window used to spot the "XHDU" start-of-transfer magic.
static uint8_t s_win[4];

// Overall cap on a single transfer's wall-clock duration, on top of the
// per-byte/per-frame timeouts below (which each reset on every byte received).
// Without this, a link that keeps trickling isolated bytes just inside each
// per-byte window -- a flaky USB-serial adapter, say -- could hold the read
// loops open indefinitely with I2C2/EXTI4_15 masked (see bl_uart_do_transfer)
// and no other watchdog to recover the board short of a power cycle. 180s is
// generous for a full 44KB image at any plausible baud/retry rate.
#define BL_UART_MAX_TRANSFER_MS  180000u
static uint32_t s_xfer_deadline;

// True once s_xfer_deadline (set at the start of a transfer) has passed.
// Wraparound-safe the same way the per-byte timeout checks below are.
static bool uart_xfer_deadline_passed(void)
{
    return (int32_t)(HAL_GetTick() - s_xfer_deadline) >= 0;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Spin-read exactly `n` bytes into `dst`. `timeout_ms` is an inter-byte timeout
// (reset on every byte received), so a slow-but-progressing host is fine while a
// stalled one aborts. Returns false on timeout -- caller then bails out of the
// transfer with the app-invalid flag still set (safe: board stays in bootloader).
static bool uart_read_exact(uint8_t *dst, uint32_t n, uint32_t timeout_ms)
{
    uint32_t got = 0;
    uint32_t last = HAL_GetTick();
    while (got < n) {
        int c = debug_uart_getc();
        if (c >= 0) {
            dst[got++] = (uint8_t)c;
            last = HAL_GetTick();
        } else if ((HAL_GetTick() - last) > timeout_ms || uart_xfer_deadline_passed()) {
            return false;
        }
    }
    return true;
}

// Discard everything currently in the RX ring. Called right after each reply is
// transmitted: USART2_TX activity crosstalks onto the lightly pulled-up
// USART2_RX line and the receiver mis-samples it as a burst of spurious bytes
// (~0.4 per real TX byte). Draining here plus the per-frame preamble resync
// (uart_sync_preamble) below makes the framing immune to it.
static void uart_drain(void)
{
    while (debug_uart_getc() >= 0) { }
}

// Scan the byte stream for the 2-byte frame preamble, tolerating any amount of
// leading garbage. Returns false on inter-byte timeout.
static bool uart_sync_preamble(uint32_t timeout_ms)
{
    uint8_t a = 0, b = 0;
    uint32_t last = HAL_GetTick();
    for (;;) {
        int c = debug_uart_getc();
        if (c < 0) {
            if ((HAL_GetTick() - last) > timeout_ms || uart_xfer_deadline_passed()) {
                return false;
            }
            continue;
        }
        last = HAL_GetTick();
        a = b;
        b = (uint8_t)c;
        if (a == BL_PRE0 && b == BL_PRE1) {
            return true;
        }
    }
}

// Host <- MCU replies are plain "XHD*\r\n" text via debug_log(), sharing the wire
// with the human [BL UART] log; the host ignores any line not starting with
// "XHD". Host -> MCU frames are binary and preamble-framed (see uart_sync_preamble).
static void bl_uart_transfer_body(void)
{
    s_xfer_deadline = HAL_GetTick() + BL_UART_MAX_TRANSFER_MS;

    uint8_t hdr[10]; // ver:u8 | flags:u8 | img_size:u32 | img_crc32:u32
    if (!uart_read_exact(hdr, sizeof(hdr), 2000)) {
        debug_log("XHDE 2\r\n");   // hello timeout
        return;
    }

    uint8_t  ver      = hdr[0];
    uint8_t  flags    = hdr[1];
    uint32_t img_size = rd_u32(&hdr[2]);
    uint32_t img_crc  = rd_u32(&hdr[6]);
    bool     keep_p63 = (flags & BL_FLAG_KEEP_P63) != 0;

    if (ver != BL_UART_PROTO_VER || img_size == 0 || img_size > BL_UART_MAX_IMG) {
        debug_log("[BL UART] reject ver=%u size=%lu max=%lu\r\n",
                  ver, (unsigned long)img_size, (unsigned long)BL_UART_MAX_IMG);
        debug_log("XHDE 1\r\n");
        return;
    }

    uint32_t n_pages = (img_size + BL_UART_PAGE_SIZE - 1) / BL_UART_PAGE_SIZE;

    static uint8_t page_buf[BL_UART_PAGE_SIZE];

    // Arm the app-invalid flag for the duration -- an abort/power-loss then leaves
    // the board safely in the bootloader for a retry. Programs only the flag halfword:
    // the rest of page 63 (user_config) is untouched until the image checks out.
    flash_set_flag();

    debug_log("XHDK %u %lu\r\n", (unsigned)BL_UART_PAGE_SIZE, (unsigned long)n_pages);
    uart_drain();
    uint32_t retry_budget = n_pages * 6 + 16;

    for (uint32_t i = 0; i < n_pages; ) {
        uint8_t seqb[2], crcb[4];
        if (!uart_sync_preamble(4000) ||
            !uart_read_exact(seqb, 2, 3000) ||
            !uart_read_exact(page_buf, BL_UART_PAGE_SIZE, 3000) ||
            !uart_read_exact(crcb, 4, 3000)) {
            debug_log("XHDE 3\r\n");   // frame recv timeout
            return;
        }

        uint16_t seq  = (uint16_t)(seqb[0] | (seqb[1] << 8));
        uint32_t pcrc = rd_u32(crcb);

        // Host missed an earlier ACK and resent a page we already wrote: re-ACK
        // it (without re-flashing) and don't advance -- resyncs the two counters.
        if (seq < i) {
            debug_log("XHDK %u\r\n", seq);
            uart_drain();
            continue;
        }

        uint16_t page = (uint16_t)(BL_FIRST_APP_PAGE + seq);
        bool ok = (seq == i) &&
                  (page >= BL_FIRST_APP_PAGE) && (page < BL_END_APP_PAGE) &&
                  (crc32_buf(page_buf, BL_UART_PAGE_SIZE) == pcrc) &&
                  flash_program_page_verified(page, page_buf, BL_UART_PAGE_SIZE, pcrc);

        debug_log("XHD%c %u\r\n", ok ? 'K' : 'N', seq);
        uart_drain();

        if (!ok) {
            if (retry_budget-- == 0) {
                debug_log("XHDE 4\r\n");   // retry budget exhausted
                return;
            }
            continue;   // host resends the same seq
        }
        set_led_1((i & 1) != 0); // progress: LED1 toggles per page
        i++;
    }

    uint32_t actual = crc32_flash(APP_START_ADDRESS, img_size);
    bool good = (actual == img_crc);
    debug_log("XHDD %u %08lX\r\n", good ? 0u : 1u, (unsigned long)actual);

    if (good) {
        // Clear the app-invalid flag. --keep-config keeps the rest of page 63 (user_config:
        // encoder self-latch, calibration, settings); the default erases the whole page, so
        // the application starts from factory defaults and re-detects the encoder.
        if (keep_p63) {
            flash_remove_flag();
        } else {
            flash_erase_page(LAST_FLASH_PAGE);
        }
        debug_log("[BL UART] DONE OK (%s) -- resetting into app\r\n",
                  keep_p63 ? "config kept" : "page 63 wiped");
        HAL_Delay(50);
        NVIC_SystemReset();
    } else {
        debug_log("[BL UART] DONE FAIL want=%08lX got=%08lX -- staying in bootloader\r\n",
                  (unsigned long)img_crc, (unsigned long)actual);
    }
}

static void bl_uart_do_transfer(void)
{
    // USART2 RX sits at NVIC priority 3 -- below the I2C2 slave ISR (SMBus
    // flasher, prio 1) and the EXTI4_15 ADV7511 ISR (prio 2, enabled in the
    // app-invalid recovery loop). USART2 also runs with overrun-discard (OVRDIS),
    // so a byte silently vanishes if either preempts the reader for more than one
    // character time. Neither is relevant during a UART flash: give USART2 RX top
    // priority and mask the other two. Restored on every failure path; the
    // success path resets the MCU and never returns.
    // EXTI4_15 (ADV_IRQ_CHANNEL) is only ever enabled by enter_bootloader_mode()'s
    // recovery loop -- enter_bootloader_mode_fw() deliberately never touches it, to
    // leave the ADV chip and its interrupt state completely untouched (see that
    // function's comment in main.c). Capture whatever state it was actually in
    // before masking it here, so a failed/aborted transfer restores that same
    // state instead of unconditionally unmasking an interrupt the fw-triggered
    // path never armed.
    uint32_t exti_was_enabled = __NVIC_GetEnableIRQ(EXTI4_15_IRQn);

    HAL_NVIC_SetPriority(USART2_IRQn, 0, 0);
    HAL_NVIC_DisableIRQ(I2C2_IRQn);
    HAL_NVIC_DisableIRQ(EXTI4_15_IRQn);

    bl_uart_transfer_body();

    if (exti_was_enabled) {
        HAL_NVIC_EnableIRQ(EXTI4_15_IRQn);
    }
    HAL_NVIC_EnableIRQ(I2C2_IRQn);
    HAL_NVIC_SetPriority(USART2_IRQn, 3, 0);
}

void bl_uart_update_poll(void)
{
    int c;
    while ((c = debug_uart_getc()) >= 0) {
        s_win[0] = s_win[1];
        s_win[1] = s_win[2];
        s_win[2] = s_win[3];
        s_win[3] = (uint8_t)c;
        if (s_win[0] == 'X' && s_win[1] == 'H' && s_win[2] == 'D' && s_win[3] == 'U') {
            bl_uart_do_transfer();
            s_win[0] = s_win[1] = s_win[2] = s_win[3] = 0;
            return;
        }
    }
}
