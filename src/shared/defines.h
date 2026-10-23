#pragma once

// ============================================================================
// MEMORY
// ============================================================================

#define RAM_START_ADDRESS       0x20000000
#define FLASH_START_ADDRESS     0x08000000
#define FLASH_TOTAL_SIZE        0x10000
#define FLASH_PAGE_SHIFT        10
// Page 63: the saved config (user_config.h) and, in its last 2 bytes, the app-invalid flag.
#define LAST_FLASH_PAGE         ((FLASH_TOTAL_SIZE >> FLASH_PAGE_SHIFT) - 1)

#define BOOTLOADER_MAGIC_VALUE      0xDEADBEEF
#define BOOTLOADER_FLAG_ADDRESS     ((volatile uint32_t*)(RAM_START_ADDRESS + 0xf0))

// Bootloader and application addresses (20KB bootloader)
#define BOOTLOADER_SIZE           0x5000  // 20KB
#define APP_START_ADDRESS         (FLASH_START_ADDRESS + BOOTLOADER_SIZE)
#define APP_SIZE_BYTES            (FLASH_TOTAL_SIZE - BOOTLOADER_SIZE)  // 44KB
#define APP_INVALID_FLAG          0x5A5A
#ifndef APP_INVALID_FLAG_ADDRESS   // overridable only for host tests (see XHD_CONFIG_FLASH_ADDR)
#define APP_INVALID_FLAG_ADDRESS  (APP_START_ADDRESS + APP_SIZE_BYTES - 2)
#endif

// ============================================================================
// INTERRUPTS
// ============================================================================

// ADV7511 INT is on PF7, routed to EXTI4_15
#define ADV_IRQ_HANDLER EXTI4_15_IRQHandler
#define ADV_IRQ_CHANNEL EXTI4_15_IRQn

// ============================================================================
// I2C
// ============================================================================

#define I2C_SLAVE_ADDR 0x69
#define I2C_WRITE_BIT 0x80

// Read Actions
#define I2C_HDMI_COMMAND_READ_CONFIG 0 // Read value from config at current bank + index (post increments)
#define I2C_HDMI_COMMAND_READ_VERSION1 1 // Read version byte 1
#define I2C_HDMI_COMMAND_READ_VERSION2 2 // Read version byte 2
#define I2C_HDMI_COMMAND_READ_VERSION3 3 // Read version byte 3
#define I2C_HDMI_COMMAND_READ_VERSION4 4 // Read version byte 4
#define I2C_HDMI_COMMAND_READ_MODE 5 // Read current mode (1=Bootloader 2=Application)

#define I2C_HDMI_COMMAND_READ_RAM 6 // Read value from ram buffer at bank + index (post increments)
#define I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC1 7 // Read crc byte 1 (from ram buffer)
#define I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC2 8 // Read crc byte 2 (from ram buffer)
#define I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC3 9 // Read crc byte 3 (from ram buffer)
#define I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC4 10 // Read crc byte 4 (from ram buffer)
#define I2C_HDMI_COMMAND_READ_COLOR_SPACE 11 // Read the colour space setting (adv_color_space, types.h: 0=Auto, 1=RGB Full, 2=RGB Limited, 3=YCbCr passthrough, 4=RGB passthrough, 5=Auto + SD RGB Full)
#define I2C_HDMI_COMMAND_READ_EDID 12 // Read byte from cached raw EDID (both blocks) at current edid index (post increments)
#define I2C_HDMI_COMMAND_READ_PHASE 13 // Read active clock-phase trim index (0-7, from user config -- mirrors 0xBA[7:5])
// 14: retired (packed status byte) -- PLL/HDMI/BIOS and the wire VIC are in READ_VIDEO_INFO (22).
#define I2C_HDMI_COMMAND_READ_TELEMETRY 15 // Read one byte of the telemetry struct at an auto-incrementing cursor (wraps at struct_size). See doc/SMBUS.md sec 8.
#define I2C_HDMI_COMMAND_READ_ASPECT_RATIO 16 // Read active aspect ratio (0=Auto, 1=16:9, 2=4:3)
#define I2C_HDMI_COMMAND_READ_SCAN_MODE 17 // Read active scan mode (0=Auto, 1=TV overscan, 2=PC underscan)
// CEC relay: the console's own BIOS/homebrew CEC stack sends and reads raw frames through
// these. X-HD's own CEC (One Touch Play, a few automatic replies, <Standby>) is in adv7511.c.
#define I2C_HDMI_COMMAND_READ_CEC_STATUS 18 // [0]=Tx busy [1]=Tx last OK [2]=Tx last arb-lost [3]=Tx last retry-timeout [4]=Rx frame pending [5]=CEC clock output on (WRITE 156) [6]=One Touch Play on (WRITE 157) [7]=Standby at shutdown on (WRITE 162)
#define I2C_HDMI_COMMAND_READ_CEC_RX_LEN 19 // Length (1-16) of the pending Rx frame, 0 if none
#define I2C_HDMI_COMMAND_READ_CEC_RX_DATA 20 // Read one byte of the pending Rx frame at an auto-incrementing cursor (header first)
// HDMI SPD InfoFrame name (24 bytes: [0..7] vendor, [8..23] product, printable ASCII, space-
// padded). Read and staging-write cursors are separate and wrap/cap at 24; WRITE_SPD_APPLY
// value 0 resets both cursors without committing.
#define I2C_HDMI_COMMAND_READ_SPD_DATA 21 // Read one byte of the live SPD name at an auto-incrementing cursor (wraps at 24)
// Video info snapshot (XhdVideoInfo in application/smbus_i2c.h): what the console sends and
// what X-HD applied -- resolution, exact refresh, wire VIC, colour space. WRITE 151 = 1 latches
// a consistent copy and resets the cursor; READ 22 then streams it one byte per read. Poll
// cheaply by latching and reading only the first 3 bytes (version, size, change counter).
#define I2C_HDMI_COMMAND_READ_VIDEO_INFO 22
#define I2C_HDMI_COMMAND_READ_ABOUT 23 // Read one byte of the about block (XhdAbout: chip ID, MCU UID, git hash, build date) at an auto-incrementing cursor
#define I2C_HDMI_COMMAND_READ_LIVE_FLAGS 24 // [0]=audio mute held (WRITE 158) [1]=SMBus freeze on (WRITE 159)
#define I2C_HDMI_COMMAND_READ_USER_CONFIG 25 // Read one byte of the live user config (page-63 layout, user_config.h) at an auto-incrementing cursor (wraps); WRITE 160 sets it
// Wire-VIC mode and picture position are read through READ_VIDEO_INFO (flags 0x80, pos_h/pos_v).

// Write Actions
#define I2C_HDMI_COMMAND_WRITE_CONFIG 128 // Write value to config buffer at current bank + index (post increments)
#define I2C_HDMI_COMMAND_WRITE_CONFIG_BANK 129 // Write config buffer bank (sets index to 0)
#define I2C_HDMI_COMMAND_WRITE_CONFIG_INDEX 130 // Write config buffer index
#define I2C_HDMI_COMMAND_WRITE_CONFIG_APPLY 131 // Applies config when value is 1
#define I2C_HDMI_COMMAND_WRITE_SET_MODE 132 // Reboots to Bootloader or Application (1=Bootloader 2=Application) if not already in that mode

#define I2C_HDMI_COMMAND_WRITE_READ_PAGE 133 // Reads page using given value into ram buffer
#define I2C_HDMI_COMMAND_WRITE_RAM 134 // Write value to ram buffer at bank + index (post increments)
#define I2C_HDMI_COMMAND_WRITE_RAM_BANK 135 // Write ram buffer bank (sets index to 0)
#define I2C_HDMI_COMMAND_WRITE_RAM_INDEX 136 // Write ram buffer index
#define I2C_HDMI_COMMAND_WRITE_RAM_APPLY 137 // Applies ram buffer to page with value also erases + updates crc (validates page for current mode)

#define I2C_HDMI_COMMAND_WRITE_APP_FLASH_MODE 138 // 0 to mark flash ended, 1 to mark flashing began
#define I2C_HDMI_COMMAND_WRITE_COLOR_SPACE 139 // Write the colour space setting (values as READ_COLOR_SPACE)
#define I2C_HDMI_COMMAND_WRITE_EDID_INDEX 140 // Set read index (0-255) for I2C_HDMI_COMMAND_READ_EDID
// Control suite -- ISR stashes into user config + raises a dirty bit only; the main loop
// (smbus_consume_settings_changes()) does all register / flash / reapply work off-ISR.
#define I2C_HDMI_COMMAND_WRITE_PHASE 141 // Set clock-phase trim index (0-7): live to 0xBA[7:5] + user config
#define I2C_HDMI_COMMAND_WRITE_CONFIG_ACTION 142 // 1=persist current user config to flash page 63, 2=restore factory defaults (persist + reapply), 3=force reapply of active mode, 4=reload the saved config from flash (+ reapply)
// 143 unused (was WRITE_CONFIG_DEFAULTS, folded into WRITE_CONFIG_ACTION above)
#define I2C_HDMI_COMMAND_WRITE_ASPECT_RATIO 144 // 0=Auto, 1=16:9, 2=4:3 (user config + mode reapply)
#define I2C_HDMI_COMMAND_WRITE_SCAN_MODE 145 // 0=Auto, 1=TV overscan, 2=PC underscan (user config + mode reapply)
// CEC relay writes -- see the READ_CEC_* opcodes' own comment above.
#define I2C_HDMI_COMMAND_WRITE_CEC_TX_DATA 146 // Stage one Tx frame byte (header first) at an auto-incrementing cursor, reset by the previous WRITE_CEC_TX_APPLY
#define I2C_HDMI_COMMAND_WRITE_CEC_TX_APPLY 147 // value = total frame length (1-16); sends the staged bytes, then resets the staging cursor
#define I2C_HDMI_COMMAND_WRITE_CEC_RX_ACK 148 // value=1: free the pending Rx frame (whether or not it was read) for the next one
// SPD name writes -- see READ_SPD_DATA's comment above.
#define I2C_HDMI_COMMAND_WRITE_SPD_DATA 149 // Stage one name byte at an auto-incrementing cursor (0-23)
#define I2C_HDMI_COMMAND_WRITE_SPD_APPLY 150 // 1 = commit all 24 staged bytes (rejected unless all 24 staged + printable) and resend SPD; 0 = reset cursors only. Persist with WRITE_CONFIG_ACTION=1
#define I2C_HDMI_COMMAND_WRITE_VIDEO_INFO_LATCH 151 // 1 = latch the live video info for READ_VIDEO_INFO (22) and reset its cursor
#define I2C_HDMI_COMMAND_WRITE_WIRE_VIC_MODE 152 // 0 = honest, 1 = chip-auto; reapplies the current mode. Persist with WRITE_CONFIG_ACTION=1
#define I2C_HDMI_COMMAND_WRITE_POSITION_H 153 // Signed offset (pixels, -127..127) for the active mode, applied live. Persist with WRITE_CONFIG_ACTION=1
#define I2C_HDMI_COMMAND_WRITE_POSITION_V 154 // Signed offset (lines, -127..127) for the active mode, applied live. Persist with WRITE_CONFIG_ACTION=1
#define I2C_HDMI_COMMAND_WRITE_ABOUT_INDEX 155 // Set the READ_ABOUT cursor (0 = start)
#define I2C_HDMI_COMMAND_WRITE_CEC_CLOCK_OUT 156 // 1 = drive CEC_CLK (12 MHz) from the MCU on PA8 / pin 29 (wired to ADV7511 pin 50; default), 0 = off. Persist with WRITE_CONFIG_ACTION=1
#define I2C_HDMI_COMMAND_WRITE_CEC_ONE_TOUCH 157 // 1 = CEC One Touch Play once per boot (wake the TV, switch it to this input; default), 0 = off. Persist with WRITE_CONFIG_ACTION=1
#define I2C_HDMI_COMMAND_WRITE_AUDIO_MUTE 158 // 1 = mute HDMI audio and hold it through mode changes, 0 = release (unmute). Not persisted
#define I2C_HDMI_COMMAND_WRITE_FREEZE 159 // 1 = ignore the console's mode packets (WRITE_CONFIG_APPLY), 0 = off (default). Persist with WRITE_CONFIG_ACTION=1 to apply at boot
#define I2C_HDMI_COMMAND_WRITE_USER_CONFIG_INDEX 160 // Set the READ_USER_CONFIG cursor
#define I2C_HDMI_COMMAND_WRITE_TELEMETRY_INDEX 161 // Set the READ_TELEMETRY cursor
#define I2C_HDMI_COMMAND_WRITE_CEC_STANDBY 162 // 1 = send the TV CEC <Standby> when the console shuts down (default), 0 = off. Persist with WRITE_CONFIG_ACTION=1

// Release version, read over SMBus opcodes 1-4 (XHDUpdater, `v`). VERSION1 stays 0 (the
// original project's 0.1.8.0 already used it that way); VERSION2-4 = major.minor.patch.
// Never FF FF FF FF -- XHDUpdater reads that as "no X-HD".
#define I2C_HDMI_VERSION1 0
#define I2C_HDMI_VERSION2 2
#define I2C_HDMI_VERSION3 0
#define I2C_HDMI_VERSION4 0

#define I2C_HDMI_MODE_BOOTLOADER 1
#define I2C_HDMI_MODE_APPLICATION 2

// ============================================================================
// SMBUS
// ============================================================================

#define SMBUS_SMS_READY          ((uint32_t)0x00000001)  /*!< No operation ongoing */
#define SMBUS_SMS_TRANSMIT       ((uint32_t)0x00000002)  /*!< State of writing data to the bus */
#define SMBUS_SMS_RECEIVE        ((uint32_t)0x00000004)  /*!< State of receiving data on the bus */
#define SMBUS_SMS_RESPONSE_READY ((uint32_t)0x00000010)  /*!< Slave has reply ready for transmission */

#define RAM_BUFFER_SIZE 1024
