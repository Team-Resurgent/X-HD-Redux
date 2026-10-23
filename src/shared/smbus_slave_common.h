#pragma once
// The Xbox-facing SMBus slave, shared by the application and the bootloader: the I2C2 hardware
// and the transaction framing (a command byte, then either a response byte the master reads or
// a data byte it writes). Each image supplies the three hooks below with its own command set.

#include <stdint.h>
#include "stm32.h"

// I2C2 as the slave at I2C_SLAVE_ADDR on PB10 (SCL) / PB11 (SDA), interrupt at priority 1,
// listening.
void smbus_slave_init(void);

// WRITE_SET_MODE: set the boot flag for `mode` (I2C_HDMI_MODE_BOOTLOADER/_APPLICATION; any
// other value leaves it), let the write's ACK finish, and reset. Called from the I2C2 ISR.
void smbus_set_mode_and_reset(uint8_t mode);

// Per-image hooks, all called from the I2C2 ISR (priority 1): RAM only -- no I2C1, nothing
// slow, the console's clock is held while they run.
// Addressed at I2C_SLAVE_ADDR, either direction.
void smbus_slave_on_address(void);
// A command byte arrived: the response a read of `cmd` returns (write commands get one too,
// never sent). `prev` is the previous response, for a command that leaves it as it was.
uint8_t smbus_slave_read(uint8_t cmd, uint8_t prev);
// A write command's data byte arrived.
void smbus_slave_write(uint8_t cmd, uint8_t value);

// The paged buffers' cursor (config and RAM): advance the index, wrapping at 256 into the bank.
static inline void smbus_paged_index_advance(uint16_t *bank, uint16_t *index)
{
    if (++*index > 0xff) {
        *index = 0;
        (*bank)++;
    }
}
