#include "smbus_i2c.h"
#include "stm32.h"
#include "../shared/debug.h"
#include "../shared/defines.h"
#include "../shared/flash.h"
#include "../shared/crc32.h"
#include "../shared/smbus_slave_common.h"

// The bootloader's SMBus commands: the mode switch, identification and XHDUpdater's page
// transfer. The framing is smbus_slave_common.c's; these are its hooks.

static uint16_t ram_buffer_bank = 0;
static uint16_t ram_buffer_index = 0;
static uint8_t ram_buffer[RAM_BUFFER_SIZE];
static uint32_t ram_buffer_crc = 0;

// -------------------- Initialization --------------------
void smbus_i2c_init(void)
{
    smbus_slave_init();
    debug_log("SMBus: I2C Slave 0x%02X ready\r\n", I2C_SLAVE_ADDR);
}

void smbus_slave_on_address(void)
{
}

// A command byte's read response; 0xFF for anything the bootloader doesn't answer.
uint8_t smbus_slave_read(uint8_t cmd, uint8_t prev)
{
    (void)prev;
    switch(cmd)
    {
        case I2C_HDMI_COMMAND_READ_MODE:
        {
            return I2C_HDMI_MODE_BOOTLOADER;
        }
        // XHDUpdater only treats the board as an X-HD once these four bytes read as
        // something other than 0xFFFFFFFF (Utility::GetHdmiDevice()); its bootloader-
        // mode branch then flashes without asking the application for anything. A
        // board left here by an interrupted update (app-invalid flag set) is only
        // recoverable over SMBus if the bootloader answers them too.
        case I2C_HDMI_COMMAND_READ_VERSION1:
        case I2C_HDMI_COMMAND_READ_VERSION2:
        case I2C_HDMI_COMMAND_READ_VERSION3:
        case I2C_HDMI_COMMAND_READ_VERSION4:
        {
            static const uint8_t version[4] = {
                I2C_HDMI_VERSION1, I2C_HDMI_VERSION2, I2C_HDMI_VERSION3, I2C_HDMI_VERSION4
            };
            return version[cmd - I2C_HDMI_COMMAND_READ_VERSION1];
        }
        case I2C_HDMI_COMMAND_READ_RAM:
        {
            uint16_t ram_buffer_offset = (ram_buffer_bank << 8) | ram_buffer_index;
            if (ram_buffer_offset >= RAM_BUFFER_SIZE)
            {
                return 0xFF;
            }
            const uint8_t r = ram_buffer[ram_buffer_offset];
            smbus_paged_index_advance(&ram_buffer_bank, &ram_buffer_index);
            return r;
        }
        case I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC1:
        case I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC2:
        case I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC3:
        case I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC4:
        {
            // Big-endian: CRC1 is bits 31:24 ... CRC4 bits 7:0.
            return (uint8_t)(ram_buffer_crc >> (8u * (I2C_HDMI_COMMAND_READ_RAM_PAGE_CRC4 - cmd)));
        }
        default:
        {
            return 0xFF;
        }
    }
}

void smbus_slave_write(uint8_t cmd, uint8_t value)
{
    switch(cmd)
    {
        case I2C_HDMI_COMMAND_WRITE_SET_MODE:
        {
            smbus_set_mode_and_reset(value);
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_READ_PAGE:
        {
            // value is caller-controlled (0-255) with no other validation --
            // flash_copy_page()/crc32_copy() dereference it as a raw pointer with
            // no bounds checking of their own. Any page within the physical flash
            // (0-63, including the bootloader's own region -- reading it back for
            // verification is legitimate, unlike writing) is fine; anything higher
            // multiplies out past the actual flash and reads undefined memory.
            if (value >= (FLASH_TOTAL_SIZE >> FLASH_PAGE_SHIFT))
            {
                break;
            }
            ram_buffer_crc = flash_copy_page(value, ram_buffer, RAM_BUFFER_SIZE);
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_RAM:
        {
            uint16_t ram_offset = (ram_buffer_bank << 8) | ram_buffer_index;
            if (ram_offset >= RAM_BUFFER_SIZE)
            {
                break;
            }
            ram_buffer[ram_offset] = value;
            smbus_paged_index_advance(&ram_buffer_bank, &ram_buffer_index);
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_RAM_BANK:
        {
            ram_buffer_bank = value;
            ram_buffer_index = 0;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_RAM_INDEX:
        {
            ram_buffer_index = value;
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_RAM_APPLY:
        {
            // Reject pages inside the bootloader's own region and any page past
            // the physical flash size -- either would erase/write out of range.
            if (value < (BOOTLOADER_SIZE >> FLASH_PAGE_SHIFT) ||
                value >= (FLASH_TOTAL_SIZE >> FLASH_PAGE_SHIFT))
            {
                break;
            }
            // Shared erase+write+CRC-verify path (also used by the UART
            // updater). expect_crc must be the CRC of the data being written
            // *now* -- ram_buffer_crc is stale here (it's only ever set by
            // WRITE_READ_PAGE reading a possibly-unrelated page from flash),
            // so compute it fresh over the buffer this call actually flashes.
            // Return value unchecked here to preserve the prior behaviour --
            // the SMBus host verifies separately via READ_RAM_PAGE_CRC.
            flash_program_page_verified(value, ram_buffer, RAM_BUFFER_SIZE,
                                         crc32_buf(ram_buffer, RAM_BUFFER_SIZE));
            break;
        }
        case I2C_HDMI_COMMAND_WRITE_APP_FLASH_MODE:
        {
            if (value == 0)
            {
                flash_remove_flag();
            }
            else
            {
                flash_set_flag();
            }
            break;
        }
    }
}
