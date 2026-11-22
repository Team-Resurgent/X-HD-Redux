// SPDX-License-Identifier: GPL-3.0-only
#ifndef __XBOX_TIMING_ROWS_H__
#define __XBOX_TIMING_ROWS_H__

#include <stdbool.h>
#include <stdint.h>

// The per-encoder mode tables (xbox_timing_rows.c). The bootloader carries them in its last
// 512 bytes (bootloader.ld's TIMING region) and the application reads them from there
// (XHD_TIMING_TABLES_EXTERNAL), so they exist once in flash. The config XBE writes the
// bootloader before the application whenever its pages differ, so a new application finds
// its tables there. An application-only flash (UART updater, ST-Link application env) on a
// board whose bootloader is older, or whose tables differ, runs without them / on the old
// values: update both. Bump the version whenever xbox_timing_tables_t's layout changes.
#define XBOX_TIMING_TABLES_ADDR     0x08004E00u
#define XBOX_TIMING_TABLES_MAGIC    0x52544858u   // "XHTR"
#define XBOX_TIMING_TABLES_VERSION  1u

// One encoder's timing for one row.
typedef struct { uint16_t delay_hs; uint8_t delay_vs, hsync_placement, hsync_duration, vsync_placement, vsync_duration; } EncTiming;
// A row's raster and its VIC (4:3, 16:9). VIC 4/5 rows are HD: sync-adjust off, and VIC 5
// (1080i) interlaced.
typedef struct { uint16_t active_w, active_h; uint8_t vic_4_3, vic_16_9; } RowShape;

// Every mode_index's row. Several share one: 0x07/0x11 use 0x08's (0x11 framed to 640 in
// xbox_get_video_timing()), 0x0B 0x0A's, 0x0D/0x0E
// 0x0C's (0x0C alone is 1440 wide, set in code), 0x14 (standalone, interlaced NTSC 640)
// 0x01's. Xcalibur's 0x07 is 640-wide: row 0, set in code. 0xFF = no such mode.
enum { ROW_0x01, ROW_0x02, ROW_0x03, ROW_0x04, ROW_0x05, ROW_0x06, ROW_0x08, ROW_0x09, ROW_0x0A,
       ROW_0x0C, ROW_0x0F, ROW_0x10, ROW_0x12, ROW_0x13, ROW_0x15, ROW_COUNT };

typedef struct {
    uint32_t  magic;              // XBOX_TIMING_TABLES_MAGIC
    uint16_t  version;            // XBOX_TIMING_TABLES_VERSION
    uint16_t  size;               // sizeof(xbox_timing_tables_t)
    uint8_t   mode_row[0x16];     // mode_index -> row, 0xFF = no such mode
    RowShape  shape[ROW_COUNT];
    EncTiming rows[ROW_COUNT][3]; // column order: XCALIBUR, CONEXANT, FOCUS
} xbox_timing_tables_t;

// Checks the tables once, at boot (the application, before any mode): false if they are
// missing or of another layout -- xbox_timing_tables() is then NULL and xbox_base_timing()
// finds no mode.
bool xbox_timing_tables_init(void);
// The tables, or NULL (above). The bootloader's own are always there.
const xbox_timing_tables_t *xbox_timing_tables(void);
// Whether t holds tables of this build's layout.
bool xbox_timing_tables_ok(const xbox_timing_tables_t *t);
// No tables this boot (the application mid HD+ -> X-HD conversion, with the HD+ bootloader
// still in pages 0-19; or an application/bootloader pair out of step). The supervisor then
// goes hands-off: no ADV7511 write at all, so the chip keeps the previous firmware's live
// configuration and its picture. Constant for a boot -- the conversion ends in an SMC
// power cycle, and the next boot finds the tables.
bool xbox_timing_tables_missing(void);

#endif // __XBOX_TIMING_ROWS_H__
