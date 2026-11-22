// SPDX-License-Identifier: GPL-3.0-only
#ifndef __I2C1_H__
#define __I2C1_H__

#include <stdbool.h>
#include <stdint.h>

// I2C1 master transfers to the ADV7511's maps and its EDID memory: `dev` the 8-bit bus
// address (0x72, 0x78, 0x70, 0x7E), one register-address byte, then `len` bytes (1-254)
// written or read. Polled on the peripheral's registers (i2c1.c). false on a NACK, a bus
// error or `timeout_ms` passing; the bus is left idle either way. I2C1 itself is set up by
// adv7511_i2c_init().
bool i2c1_write(uint8_t dev, uint8_t reg, const uint8_t *buf, uint8_t len, uint32_t timeout_ms);
bool i2c1_read(uint8_t dev, uint8_t reg, uint8_t *buf, uint8_t len, uint32_t timeout_ms);

#endif // __I2C1_H__
