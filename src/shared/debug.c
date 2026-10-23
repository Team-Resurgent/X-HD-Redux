#include "debug.h"
#include "stm32.h"
#include "adv7511.h"
#include "adv7511_i2c.h"
#include "adv7511_xbox.h"
#include "adv7511_edid.h"
#include "user_config.h"
#include "xbox_video_timing.h"
#include "types.h"

#include <string.h>
#include <stdarg.h>
#include <stddef.h>

#ifdef DEBUG_OUT

#ifdef BUILD_APPLICATION
// The CLI and the `d` extras are application-only, as are the calls they make.
#include "../application/smbus_i2c.h"        // bridge, sniff, refresh; smbus_probe.h's getters
#include "../application/xbox_video_bios.h"  // set_video_mode_bios() for `m`
#endif
#include "xbox_eeprom_video.h" // pure decode, no HAL -- safe to link into bootloader too

// ISR-context log lines (debug_ring_log) wait here until the main loop flushes them; a burst
// of SMBus packets while the main loop is busy has to fit.
#define RING_BUFFER_SIZE 512
#define RX_BUFFER_SIZE 128
#define CLI_CMD_MAX_LEN 64

typedef struct {
    char buffer[RING_BUFFER_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
} RingBuffer;

static RingBuffer debugBuffer = {0};
static volatile bool s_ring_dropped = false;   // debug_ring_log() had to drop a line
static volatile uint8_t rx_buffer[RX_BUFFER_SIZE] = {0};
static volatile uint8_t rx_head = 0;
static volatile uint8_t rx_tail = 0;
// Tick of the last byte the ISR accepted -- the CLI's partial-line purge measures line
// silence from byte arrival, not from when the main loop last got around to polling.
static volatile uint32_t rx_last_tick = 0;

#ifdef BUILD_APPLICATION
static char cli_line[CLI_CMD_MAX_LEN] = {0};
static uint8_t cli_pos = 0;
#endif

__attribute__((used)) void USART2_IRQHandler(void)
{
    while (USART2->ISR & USART_ISR_RXNE) {
        // Error flags reflect the byte currently in RDR, not the whole batch this ISR
        // entry might drain -- check per-byte, before consuming RDR, rather than once
        // for the whole loop. A glitched byte on the line (line noise, or electrical
        // activity from a flash/reset/encoder-probe cycle) would otherwise still get
        // enqueued as if it were real input -- one stray byte landing on a single-letter
        // command alias ('h', '?') is enough to execute a real CLI command.
        bool had_error = (USART2->ISR & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) != 0;
        uint8_t ch = (uint8_t)(USART2->RDR & 0xFF); // must always read RDR to clear RXNE
        if (had_error) {
            USART2->ICR = 0xFFFFFFFF;
            continue;
        }
        uint8_t next = (uint8_t)((rx_head + 1) % RX_BUFFER_SIZE);
        if (next != rx_tail) {
            rx_buffer[rx_head] = ch;
            rx_head = next;
            rx_last_tick = HAL_GetTick();
        }
    }
}

void debug_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};

    // PA2 = USART2_TX, PA3 = USART2_RX (AF1)
    GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF1_USART2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // Configure USART2: 115200 baud, Overrun Disabled (OVRDIS). BRR is computed from the
    // actual current PCLK1 rather than a hardcoded constant: debug_init() is called twice
    // in main() around SystemClock_Config() (once before, as a diagnostic safety net so a
    // failure in there isn't silent; once after, to finalize for whatever clock that call
    // actually lands on), and a fixed constant can only be correct for one of those two
    // PCLK1 values.
    USART2->CR1 &= ~USART_CR1_UE;
    USART2->BRR = (uint16_t)(HAL_RCC_GetPCLK1Freq() / 115200u);
    USART2->CR3 |= USART_CR3_OVRDIS;
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;
    USART2->ICR = 0xFFFFFFFF;
    (void)USART2->RDR;

    // Priority 3, below EXTI4_15 (the passive SMBus probe; map in main.c). PA3/USART2_RX only
    // has the MCU's internal ~40k pull-up and picks up real noise during the console's
    // EMI-heavy power-up window; a noise burst can drain several bytes per ISR entry, and
    // if USART2 outranked EXTI4_15 each such entry would be a chance to preempt the bus
    // probe mid-bit and, since EXTI->PR is a sticky flag not a counter, silently coalesce
    // away a second same-line edge before the probe ISR samples it. Same tier as
    // SysTick_IRQn (also 3): CLI responsiveness only loses a few ms under real bus-probe
    // activity, which is harmless.
    HAL_NVIC_SetPriority(USART2_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}

static char s_debug_log_buf[192];

// debug_ring_log() has its own buffer, separate from debug_log()'s s_debug_log_buf.
// debug_log() formats into its buffer then transmits it byte-by-byte via a *blocking*
// uart_putc() loop (one byte every ~87us at 115200 baud -- a large window). debug_ring_log()
// is meant to be callable from ISR context (e.g. smbus_i2c.c's I2C2 callback) concurrently
// with that slow path; if it shared debug_log()'s buffer, firing mid-transmission would
// overwrite the buffer while the outer debug_log() loop was still reading from it by index,
// splicing two messages together mid-word. Independent buffers remove that race.
static char s_debug_ring_log_buf[192];

// Minimal vsnprintf covering exactly the conversions this firmware's debug strings use:
// %s %c %d %u %x %X %%, an optional '0' flag, a decimal field width (numeric conversions
// only -- no call site ever widths a %s), and a length modifier (l/h/z) that is parsed and
// ignored -- harmless on this 32-bit ILP32 target where int/long are the same width.
// Replaces newlib-nano's ~1.4KB formatted-output engine (_printf_i / _svfiprintf_r /
// _printf_common / __ssputs_r), which nothing outside this file pulls in. Same C99 return
// contract: the length the output would have had; the buffer is always NUL-terminated when
// size > 0. (Same spirit as the strtoul -> parse_hex swap further down -- keep newlib's
// locale/format machinery out of the image.)
static int mini_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t n = 0;
    #define MVP_PUT(ch) do { if (n + 1 < size) buf[n] = (char)(ch); n++; } while (0)

    for (; *fmt; fmt++) {
        if (*fmt != '%') { MVP_PUT(*fmt); continue; }
        fmt++;
        if (*fmt == '%') { MVP_PUT('%'); continue; }
        if (*fmt == '\0') { fmt--; break; }

        // Only '0' (zero-pad) is a real flag across every debug string in this firmware --
        // no '-'/'+'/' '/'#' call site exists.
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;

        char conv = *fmt;
        if (conv == '\0') { fmt--; break; }

        char num[12];
        int nlen = 0;
        int neg = 0;

        switch (conv) {
        // No call site ever passes a field width with %s (checked) -- emit it directly
        // instead of sharing the numeric width/pad tail below, which only %c/%d/%u/%x/%X
        // still need.
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (str == NULL) str = "(null)";
            while (*str) MVP_PUT(*str++);
            continue;
        }
        case 'c':
            num[nlen++] = (char)va_arg(ap, int);
            break;
        case 'd': {
            int v = va_arg(ap, int);
            unsigned uv;
            if (v < 0) { neg = 1; uv = (unsigned)(-(long)v); } else { uv = (unsigned)v; }
            do { num[nlen++] = (char)('0' + uv % 10); uv /= 10; } while (uv);
            break;
        }
        case 'u': {
            unsigned uv = va_arg(ap, unsigned);
            do { num[nlen++] = (char)('0' + uv % 10); uv /= 10; } while (uv);
            break;
        }
        case 'x':
        case 'X': {
            unsigned uv = va_arg(ap, unsigned);
            const char *hex = (conv == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
            do { num[nlen++] = hex[uv & 0xF]; uv >>= 4; } while (uv);
            break;
        }
        default:
            MVP_PUT('%');
            MVP_PUT(conv);
            continue;
        }

        int total = nlen + (neg ? 1 : 0);
        if (neg && pad == '0') MVP_PUT('-');       // sign before zero-padding
        for (int i = total; i < width; i++) MVP_PUT(pad);
        if (neg && pad != '0') MVP_PUT('-');       // sign after space-padding
        while (nlen > 0) MVP_PUT(num[--nlen]);
    }

    if (size > 0) buf[(n < size) ? n : size - 1] = '\0';
    #undef MVP_PUT
    return (int)n;
}

// Only the application build's register-dump CLI uses this; unused in the bootloader.
__attribute__((unused))
static int mini_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = mini_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

static void uart_putc(char c)
{
    // A raw iteration-count timeout, not the tick: this also runs before SysTick starts and
    // from the fault handler. A few ms at 24 MHz, against ~87 us per byte at 115200 baud.
    uint32_t timeout = 30000;
    while (!(USART2->ISR & USART_ISR_TXE) && timeout--) {
        __NOP();
    }
    if (timeout > 0) {
        USART2->TDR = (uint8_t)c;
    }
}

void debug_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int len = mini_vsnprintf(s_debug_log_buf, sizeof(s_debug_log_buf), fmt, args);
    va_end(args);
    if (len > 0) {
        if (len >= (int)sizeof(s_debug_log_buf)) {
            len = (int)sizeof(s_debug_log_buf) - 1;
        }
        for (int i = 0; i < len; i++) {
            uart_putc(s_debug_log_buf[i]);
        }
    }
}

void debug_ring_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int len = mini_vsnprintf(s_debug_ring_log_buf, sizeof(s_debug_ring_log_buf), fmt, args);
    va_end(args);

    // vsnprintf returns the length it would have written, not what actually fit --
    // clamp like debug_log() does, or a format string over 192 chars reads past the
    // end of s_debug_ring_log_buf below.
    if (len >= (int)sizeof(s_debug_ring_log_buf)) {
        len = (int)sizeof(s_debug_ring_log_buf) - 1;
    }

    // Whole line or nothing: a line cut off where the ring filled up ran straight into the
    // next one ("SMBus: encod[AUTODETECT] ..."). A dropped line is announced by the flush.
    // The head is published only once the whole line is in.
    uint16_t head = debugBuffer.head;
    for (int i = 0; i < len; i++) {
        uint16_t next = (head + 1) % RING_BUFFER_SIZE;
        if (next == debugBuffer.tail) {
            s_ring_dropped = true;
            return;
        }
        debugBuffer.buffer[head] = s_debug_ring_log_buf[i];
        head = next;
    }
    debugBuffer.head = head;
}

void debug_ring_flush(void)
{
    while(debugBuffer.tail != debugBuffer.head)
    {
        uint16_t tail = debugBuffer.tail;
        uart_putc(debugBuffer.buffer[tail]);
        debugBuffer.tail = (tail + 1) % RING_BUFFER_SIZE;
    }
    if (s_ring_dropped) {
        s_ring_dropped = false;
        debug_puts("[LOG] lost\r\n");
    }
}

// Pop one raw byte from the USART2 RX ring, or -1 if empty. The ISR
// (USART2_IRQHandler) is the producer; this is the only consumer in the
// bootloader build (the application consumes the same ring in debug_cli_poll()), so it's
// compiled into the bootloader only.
#ifndef BUILD_APPLICATION
int debug_uart_getc(void)
{
    if (rx_head == rx_tail) {
        return -1;
    }
    uint8_t ch = rx_buffer[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1) % RX_BUFFER_SIZE);
    return (int)ch;
}
#endif

#ifdef BUILD_APPLICATION

// CRC-8 (poly 0x07, init 0) over a NUL-terminated string -- a cheap line-integrity check
// for the "[<tag>:RAW]" telemetry lines, the bridge's "[SB]"/"R" replies and incoming
// commands. xhd_tool.py's and xhd_virtual.py's _crc8() mirror this exactly.
static uint8_t crc8_str(const char *s)
{
    uint8_t crc = 0;
    for (; *s; s++) {
        crc ^= (uint8_t)*s;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// Emit "[<tag>:RAW] v<ver> <payload> *<crc8(payload)>". `fmt`/args build the payload only;
// the wrapper (tag / version / CRC / CRLF) lives here, out of every call site. xhd_tool.py's
// renderer for <tag> checks the version + CRC and falls back to echoing the raw line on any
// mismatch rather than rendering a corrupted or format-drifted line as if it were valid.
static void debug_raw_log(const char *tag, unsigned ver, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    mini_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    debug_log("[%s:RAW] v%u %s *%02X\r\n", tag, ver, buf, crc8_str(buf));
}

// Column headers, section labels, and out-of-range masking are formatted host-side by
// xhd_tool.py's render_dump(). This function's only job is the I2C burst read. Always
// emits the full 16-byte row (even columns outside [start,end]) -- cheaper than branching
// on addr here, and the host discards what it doesn't need.
static void print_dump_range(uint8_t start, uint8_t end)
{
    for (uint16_t row = (start & 0xF0); row <= end; row += 16) {
        uint8_t row_buf[16] = {0};
        I2C_HandleTypeDef* hi2c = adv7511_i2c_instance();
        if (HAL_I2C_Mem_Read(hi2c, ADV7511_MAIN_I2C_ADDR, (uint8_t)row, I2C_MEMADD_SIZE_8BIT, row_buf, 16, 20) != HAL_OK) {
            memset(row_buf, 0x00, sizeof(row_buf));
        }
        char line[64];
        int pos = mini_snprintf(line, sizeof(line), "[DUMP] %02X", (uint8_t)row);
        for (uint8_t col = 0; col < 16; col++) {
            pos += mini_snprintf(line + pos, sizeof(line) - pos, " %02X", row_buf[col]);
        }
        debug_log("%s\r\n", line);
    }
}

// Appended to `d`'s register dump: the fields xhd_tool.py's status/timing/audio/AVI views
// need that aren't ADV7511 registers. Those views are all built from one `d`, so a snapshot's
// fields come from one instant.
//   st   = state byte: bit0 active_mode_is_bios, bits[3:1] effective colour space (1-4),
//                       bit4 eeprom VideoStandard valid, bit5 VideoSettings valid,
//                       bit6 colour space configured AUTO (bits[3:1] are then the resolve)
//   vstd/vset = EEPROM VideoStandard/VideoSettings dwords (0 if not captured)
//   rr   = EEPROM-resolved dashboard refresh (50 / 60 / 0=unknown)
//   prof = timing-profile index that produced the currently applied timing (0xFF = none yet)
//   pclk = settled pixel clock in Hz (adv7511_pixel_clock_settled_hz(): a reading held for
//          200 ms, never a raw sample -- the daemon queries `d` right after mode changes,
//          when raw reads are 0 or outliers). 0 = no settled reading since the last loss.
//          Keep the literal "pclk=" token: xhd_daemon.py matches "pclk=0 " on the raw
//          response to retry.
static void print_extras(void)
{
    uint32_t vstd = 0, vset = 0;
    bool have_std = xbox_eeprom_get_video_standard(&vstd);
    bool have_set = xbox_eeprom_get_video_settings(&vset);

    uint8_t st = (adv7511_active_mode_is_bios() ? 0x01 : 0x00) |
                 (((uint8_t)adv7511_get_effective_color_space() & 0x07) << 1) |
                 (have_std ? 0x10 : 0x00) |
                 (have_set ? 0x20 : 0x00) |
                 // 0x40 = an AUTO-family policy is selected (AUTO or AUTO_SD_RGB_FULL) -- `c`
                 // with no argument names which one.
                 ((adv7511_get_color_space() == COLOR_SPACE_AUTO ||
                   adv7511_get_color_space() == COLOR_SPACE_AUTO_SD_RGB_FULL) ? 0x40 : 0x00);

    uint8_t rr = 0;
    if (have_std) {
        xbox_eeprom_refresh_t r = xbox_eeprom_resolve_refresh(vstd, have_set ? vset : 0u);
        rr = (r == XBOX_EEPROM_REFRESH_50HZ) ? 50 : (r == XBOX_EEPROM_REFRESH_60HZ) ? 60 : 0;
    }

    // ivic = internal VIC of the applied mode (what the arbiter tracks; 0x3C is the WIRE VIC
    // in honest mode), wv = user_config wire_vic_mode (0 honest, 1 chip-auto).
    debug_raw_log("EXTRA", 1, "%02X %08lX %08lX %u %u pclk=%lu ivic=%u wv=%u rf=%u",
                  st, (unsigned long)vstd, (unsigned long)vset,
                  rr, xbox_video_timing_last_profile_index(),
                  (unsigned long)adv7511_pixel_clock_settled_hz(),
                  adv7511_read_active_vic(), user_config_get()->wire_vic_mode,
                  smbus_video_refresh_mhz());
}

// The `d` command's whole payload; main.c also prints it after the boot banner.
void debug_print_full_dump(void)
{
    print_dump_range(0x00, 0x59);
    print_dump_range(0x90, 0xBA);
    print_dump_range(0xD0, 0xDC);
    print_extras();
}

// The `sn` toggle's argument. -1 = unrecognized.
// Character checks rather than strcmp() (flash): "1"/"on" -> 1, "0"/"off" -> 0.
static int8_t parse_on_off(const char *arg)
{
    if (arg[0] == '1' || (arg[0] == 'o' && arg[1] == 'n')) return 1;
    if (arg[0] == '0' || (arg[0] == 'o' && arg[1] == 'f')) return 0;
    return -1;
}

// Replaces strtoul(arg, NULL, 16) for the CLI's register/value/index args --
// drops the newlib strtoul dependency (locale/ctype machinery). No sign, no "0x"
// prefix, no error return; stops at the first non-hex character, as strtoul does
// (counting one as a 0 digit once turned "57*60" into register 0x60).
static uint32_t parse_hex_u32(const char *s)
{
    uint32_t v = 0;
    for (;; s++) {
        const char c = *s;
        uint32_t digit;
        if (c >= '0' && c <= '9')      digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
        else                           return v;
        v = (v << 4) | digit;
    }
}

// Same, for the "hs"/"vs" commands' decimal pixel/line arguments.
static uint16_t parse_dec_u16(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = (v * 10u) + (uint32_t)(*s++ - '0');
    }
    return (uint16_t)v;
}

// The command name at the start of cmd, followed by end-of-string or a space (a trailing
// argument), so "d" doesn't fire on "dx" nor "sb" on "sbx". A single-letter name also ends at
// any control byte. No "h" alias for "help" (see the "?" branch).
static bool cmd_is(const char *cmd, const char *target)
{
    const size_t n = strlen(target);
    if (strncmp(cmd, target, n) != 0) return false;
    const char c = cmd[n];   // in bounds: cmd matched n non-NUL bytes
    return c == '\0' || c == ' ' || (n == 1 && c < ' ');
}

// Every command's bad/missing-argument reply. One fixed string instead of per-command usage
// text (~150 B of flash); the full syntax is in `xhd_tool.py help`.
static void cli_err(void)
{
    debug_log("ERR: invalid arg\r\n");
}

// Shared by every command handler below that takes an argument: skip past the matched
// command name (name_len bytes) and any spaces before the argument starts.
static char *cli_arg(char *cmd, int name_len) {
    char *a = cmd + name_len;
    while (*a == ' ') a++;
    return a;
}

void debug_puts(const char *str)
{
    if (str == NULL) return;
    while (*str) {
        uart_putc(*str++);
    }
}


static __attribute__((noinline)) void execute_cli_command(char *raw_cmd, const xbox_encoder xb_encoder)
{
    // Trim: everything <= ' ' counts as whitespace (space, tab, CR, LF, stray controls).
    char *cmd = raw_cmd;
    while (*cmd && *cmd <= ' ') cmd++;
    if (*cmd == '\0') return;

    char *end = cmd + strlen(cmd) - 1;
    while (end > cmd && *end <= ' ') {
        *end-- = '\0';
    }

    // Strip leading "xhd>" or "xhd> " if echoed back by terminal or adapter loopback
    if (strncmp(cmd, "xhd>", 4) == 0) {
        cmd += 4;
        while (*cmd && *cmd <= ' ') cmd++;
    }
    if (*cmd == '\0') return;

    // Line integrity: bytes can drop or flip on this UART during a console mode-set burst.
    // xhd_daemon.py ends every command with " *CC", the CRC-8 of the text before it (as the
    // RAW lines carry). A mismatch is refused whole -- nothing runs -- and so is an `sb` line
    // without one, since it can change settings, and any line with a '*' that isn't that
    // ending: a lost byte can break the ending itself ("r 57 *60" -> "r 57*60"), and no
    // command takes a '*'. Other commands still work typed by hand.
    const size_t n = strlen(cmd);
    if (n > 4 && cmd[n - 4] == ' ' && cmd[n - 3] == '*') {
        const uint8_t want = (uint8_t)parse_hex_u32(&cmd[n - 2]);
        cmd[n - 4] = '\0';
        if (crc8_str(cmd) != want) goto crc_err;
    } else if (strchr(cmd, '*') || cmd_is(cmd, "sb")) {
        goto crc_err;
    }

    char *arg = NULL;

    // Command names are deliberately terse (flash); xhd_tool.py translates the long names
    // ("status", "info", "timing", ...) before sending. cmd_is() accepts a trailing argument.
    // ping: link health check with no I2C at all -- tells a dead UART link from a hung I2C
    // bus, which makes every other command time out. The daemon polls it.
    if (cmd_is(cmd, "ping")) {
        debug_log("PONG\r\n");
    }
#ifdef BUILD_APPLICATION
    // SMBus bridge: `sb r <op> [n]` = n reads of SMBus read opcode <op>, printed as hex,
    // 64 bytes per "[SB] ..." line; `sb w <op> <v> [v...]` = one write per value, in order
    // ("[SB] ok"). op/v hex, n decimal (default 1). Each reply line ends " *CC" (CRC-8 of the
    // text between); an opcode smbus_bridge_*() refuses answers ERR. The settings, config actions, EDID,
    // telemetry, about, SPD and CEC go through it -- xhd_daemon.py turns the old command names
    // into these, so their parsing and formatting live host-side, not in flash.
    else if (cmd_is(cmd, "sb")) {
        arg = cli_arg(cmd, 2);
        const bool write = (arg[0] == 'w');
        char *p = cli_arg(arg, 1);
        const uint8_t op = (uint8_t)parse_hex_u32(p);
        while (*p && *p != ' ') p++;
        if (write) {
            while (*p == ' ') p++;
            bool ok = true;
            while (*p) {
                ok &= smbus_bridge_write(op, (uint8_t)parse_hex_u32(p));
                while (*p && *p != ' ') p++;
                while (*p == ' ') p++;
            }
            if (!ok) { cli_err(); goto prompt; }
            debug_log("[SB] ok *%02X\r\n", crc8_str("ok"));
        } else {
            uint16_t n = parse_dec_u16(cli_arg(p, 0));
            if (n == 0) n = 1;
            char line[2 * 64 + 1];
            uint8_t k = 0;
            for (uint16_t i = 0; i < n; i++) {
                static const char hx[] = "0123456789ABCDEF";
                uint8_t b;
                if (!smbus_bridge_read(op, &b)) { cli_err(); goto prompt; }
                line[k++] = hx[b >> 4];
                line[k++] = hx[b & 0x0F];
                if (k == 2 * 64 || i + 1 == n) {
                    line[k] = '\0';
                    debug_log("[SB] %s *%02X\r\n", line, crc8_str(line));
                    k = 0;
                }
            }
        }
    }
#endif
    else if (cmd_is(cmd, "sn")) {
        arg = cli_arg(cmd, 2);
        int8_t on_off = parse_on_off(arg);
        if (on_off >= 0) {
            smbus_set_sniff(on_off == 1);
        }
        debug_log("[SNIFF] %u\r\n", smbus_get_sniff());
    }
    else if (cmd_is(cmd, "enc")) {
        // Manually force the video encoder (0=Xcalibur 1=Conexant 2=Focus). Word forms
        // (x/c/f) are normalised by xhd_tool.py. For a board whose passive probe can't
        // catch the encoder and whose BIOS sends no SMBus -- forces + self-latches to
        // page 63, applies immediately. No arg: show current.
        arg = cli_arg(cmd, 3);
        if (*arg >= '0' && *arg <= '2') {
            // Takes effect on the next main-loop pass (self-latch); "probed" shows it now.
            static const uint8_t enc_addr[3] = { ENCODER_XCALIBUR, ENCODER_CONEXANT, ENCODER_FOCUS };
            smbus_force_encoder(enc_addr[*arg - '0']);
        } else if (*arg != '\0') {
            cli_err();
            goto prompt;   // still answer with the prompt -- the daemon waits for it
        }
        debug_log("[ENC] active=%s, probed=0x%02X\r\n",
                  get_xbox_encoder_name((uint8_t)xb_encoder),
                  smbus_get_probed_encoder_addr());
    }
    else if (cmd_is(cmd, "pr")) {
        // Re-arms the passive encoder-address bus probe (smbus_probe.c) -- `enc` shows the
        // result once the console talks to its encoder chip.
        smbus_arm_encoder_probe();
        debug_log("[PROBE] armed\r\n");
    }
    // Three register ranges plus print_extras(): everything xhd_tool.py's status, timing,
    // audio and AVI views read. The ranges are a host contract (render_dump() hardcodes them);
    // change both sides together. Range 2 reaches 0xBA for 0xAF (HDMI/DVI) and 0xBA (clock
    // delay trim).
    else if (cmd_is(cmd, "d")) {
        debug_print_full_dump();
    }
    else if (cmd_is(cmd, "w")) {
        arg = cli_arg(cmd, 1);
        char *val_str = strchr(arg, ' ');
        if (val_str != NULL) {
            *val_str = '\0';
            val_str++;
            while (*val_str == ' ') val_str++;
            uint32_t reg = parse_hex_u32(arg);
            uint32_t val = parse_hex_u32(val_str);
            adv7511_write_register((uint8_t)reg, (uint8_t)val);
            uint8_t readback = adv7511_read_register((uint8_t)reg);
            debug_log("W %02X=%02X rd %02X\r\n", (uint8_t)reg, (uint8_t)val, readback);
        } else {
            cli_err();
        }
    }
    else if (cmd_is(cmd, "r")) {
        arg = cli_arg(cmd, 1);
        if (*arg != '\0') {
            uint32_t reg = parse_hex_u32(arg);
            uint8_t val = adv7511_read_register((uint8_t)reg);
            // "R <reg>=<val> *CC", both hex, CC = CRC-8 of "<reg>=<val>" -- xhd_virtual.py
            // checks it; scripts/watch_hpd.py and the GUI parse the leading form.
            char rv[6];
            mini_snprintf(rv, sizeof(rv), "%02X=%02X", (uint8_t)reg, val);
            debug_log("R %s *%02X\r\n", rv, crc8_str(rv));
        } else {
            cli_err();
        }
    }
    else if (cmd_is(cmd, "m")) {
        arg = cli_arg(cmd, 1);
        if (*arg != '\0') {
            uint32_t idx = parse_hex_u32(arg);
            // Optional second token: "4" forces the widescreen bit clear (test a 4:3 BIOS
            // packet); anything else, or omitted, keeps the widescreen bit set (unchanged
            // default -- previously the only option, hardcoded with no way to test 4:3
            // packets from this command at all).
            char *aspect_arg = arg;
            while (*aspect_arg && *aspect_arg != ' ') aspect_arg++;
            while (*aspect_arg == ' ') aspect_arg++;
            bool force_4_3 = (*aspect_arg == '4');

            uint32_t mode = ((idx & 0xFF) << 16) | (force_4_3 ? 0 : 0x10000000);
            // Synthesize a matching avinfo (bit 0 as an always-set "populated" marker, bit
            // 16 mirroring the force_4_3 choice) rather than passing 0 -- bios_resolve_mode()
            // falls back to avinfo/EEPROM when a progressive-480p mode's bit 28 reads 0, and
            // a real avinfo=0 here would let that fallback override this command's own
            // explicit 4:3/16:9 choice with whatever EEPROM says.
            uint32_t avinfo = 0x00000001u | (force_4_3 ? 0u : 0x00010000u);
            // set_video_mode_bios() logs its own "[BIOS] bad idx ..." and
            // returns false without applying anything for an invalid index -- the return
            // value must be checked so "applied!" is never printed after that failure.
            if (set_video_mode_bios(xb_encoder, mode, avinfo)) {
                // The apply mutes audio and leaves unmuting to its caller's cycle or
                // reapply wrapper; this path has neither, so unmute here.
                adv7511_enable_video();
                adv7511_unmute_audio();
                debug_log("[MODE] %02X %s\r\n", (uint8_t)idx, force_4_3 ? "4:3" : "16:9");
            }
        } else {
            cli_err();
        }
    }
    else if (cmd_is(cmd, "t")) {
        arg = cli_arg(cmd, 1);
        if (*arg == '0' || *arg == '1') {
            if (*arg == '1') adv7511_power_up_tmds(); else adv7511_power_down_tmds();
            debug_log("[TMDS] %c\r\n", *arg);
        } else {
            cli_err();
        }
    }
    else if (cmd_is(cmd, "ep")) {
        // Raw dump of what the passive EEPROM field probe has captured off the console's
        // own boot-time EEPROM readout (see smbus_probe.c) -- xhd_tool.py decodes the
        // standard name and settings flags, same RAW/tool-decodes split as tlm/freq/etc.
        // valid bit0=VideoStandard captured, bit1=VideoSettings captured; the
        // corresponding 8-hex-digit field is meaningless (not just zero) when its bit is
        // clear, since 0 is itself a real possible VideoStandard/VideoSettings value.
        uint32_t vstd = 0, vset = 0;
        uint8_t valid = 0;
        if (xbox_eeprom_get_video_standard(&vstd)) valid |= 0x01;
        if (xbox_eeprom_get_video_settings(&vset)) valid |= 0x02;
        // Passively-sniffed digital input format (always-armed probe, latest value):
        // Conexant IN_MODE[3:0] / Focus 0x92. inmode 0xFF = not seen yet, else the raw
        // nibble; rgb=1 = GPU driving RGB on the bus. (Xcalibur: never seen -- NV2A-side.)
        uint8_t inmode = 0xFF, in_rgb_flag = 0;
        bool rgb = false;
        if (xbox_encoder_input_is_rgb(&rgb, &inmode)) in_rgb_flag = rgb ? 1 : 0;
        debug_raw_log("EEPROM", 1, "valid=%02X std=%08lX set=%08lX inmode=%02X rgb=%d",
                  valid, (unsigned long)vstd, (unsigned long)vset, inmode, in_rgb_flag);
    }
    else if (cmd_is(cmd, "ed")) {
        // Bring-up: every register write the console has made to any encoder (0x8A CX /
        // 0xD4 FS / 0xE0 XCal) or the SMC (0x20), oldest first, for decoding offline against the
        // encoder datasheets. total > count means older writes have rolled off the ring.
        // The ring holds 512 (ENC_TRACE_N, smbus_probe.c); this pages through it 64 at a time
        // (ENC_TRACE_PAGE in smbus_probe.h -- keep equal) to keep the stack small. The summary
        // line comes last, once the count is known; render_encdump() doesn't mind the order.
        uint8_t a[64], r[64], v[64];
        uint16_t total = 0;
        uint16_t skip = 0;   // also doubles as the shown-count once the loop ends, since
                              // it's advanced by exactly each call's own n
        uint8_t n;
        while ((n = smbus_enc_trace(a, r, v, skip, &total)) > 0) {
            for (uint8_t i = 0; i < n; i += 8) {
                char ln[96]; int p = 0;
                for (uint8_t j = i; j < n && j < i + 8; j++) {
                    p += mini_snprintf(ln + p, sizeof(ln) - p, " %02X:%02X=%02X",
                                       a[j], r[j], v[j]);
                }
                ln[p] = '\0';
                debug_log("[ENCDUMP]%s\r\n", ln);
            }
            skip = (uint16_t)(skip + n);
        }
        // lost = writes the decoder never saw (the main loop fell a whole ring behind).
        debug_log("[ENCDUMP] n=%u total=%u lost=%u\r\n", skip, total,
                  smbus_enc_trace_lost());
        if (xbox_xcal_mode_index())
            debug_log("[ENCDUMP] xcal mode idx=%02X\r\n", xbox_xcal_mode_index());
    }
    else if (cmd_is(cmd, "rst")) {

        NVIC_SystemReset();
    }
    // No single-character "h" alias for help/"?": USART2 TX->RX crosstalk can leave a
    // corrupted "h" fragment in the CLI line buffer (an echoed "xhd> " prompt losing its
    // leading 'x'), and a subsequent command arriving prefixed with '\n' would flush that
    // stray "h" as a real command, racing the actual in-flight command's response.
    else if (strcmp(cmd, "?") == 0) {   // "help" is mapped to "?" host-side (xhd_daemon.py)
        // Full command reference lives in scripts/xhd_tool.py ('help' action) -- keeping it
        // here too would cost ~1KB of flash for text no scripted xhd_tool.py user ever
        // needs; this breadcrumb is just for a bare serial terminal.
        debug_puts("see xhd_tool help\r\n");
    }
    else {
        return;
    }

prompt:
    debug_log("\r\nxhd> ");
    return;
crc_err:
    debug_log("ERR: crc\r\n");
    goto prompt;
}

void debug_cli_poll(const xbox_encoder xb_encoder)
{
    // Reset buffer if the line was quiet for > 60ms to purge any line glitches -- a dropped
    // RX byte (framing/noise error, or the OVRDIS overrun-discard enabled in
    // debug_init()) can leave a partial line with no terminator stuck here indefinitely.
    // Quiet is measured from the last byte's arrival (ISR-stamped): a main loop busy for
    // >60ms between two bytes of one command is not a quiet line. rx_last_tick is read
    // before the tick so an ISR landing in between can't make the difference wrap.
    uint32_t last = rx_last_tick;
    if (cli_pos > 0 && (HAL_GetTick() - last > 60)) {
        cli_pos = 0;
    }

    while (rx_head != rx_tail) {
        uint8_t ch = rx_buffer[rx_tail];
        rx_tail = (uint8_t)((rx_tail + 1) % RX_BUFFER_SIZE);

        if (ch == '\r' || ch == '\n') {
            if (cli_pos > 0) {
                cli_line[cli_pos] = '\0';
                char cmd_copy[CLI_CMD_MAX_LEN];
                strncpy(cmd_copy, cli_line, sizeof(cmd_copy) - 1);
                cmd_copy[sizeof(cmd_copy) - 1] = '\0';
                cli_pos = 0;
                execute_cli_command(cmd_copy, xb_encoder);
            }
        } else if (ch == '\b' || ch == 0x7F) {
            if (cli_pos > 0) {
                cli_pos--;
            }
        } else if (ch >= 32 && ch <= 126) {
            if (cli_pos < (CLI_CMD_MAX_LEN - 1)) {
                cli_line[cli_pos++] = (char)ch;
            }
        }
    }
}

#else

void debug_cli_poll(const xbox_encoder xb_encoder)
{
    (void)xb_encoder;
}

#endif // BUILD_APPLICATION

#endif // DEBUG_OUT
