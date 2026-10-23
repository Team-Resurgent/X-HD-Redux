#pragma once

#include <stdbool.h>
#include <stdint.h>

// Reflected CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF) --
// identical to zlib.crc32 / Python's binascii.crc32, so host tooling can match it
// with no custom implementation.

// CRC a RAM (or flash) buffer in place, no copy.
uint32_t crc32_buf(const uint8_t *data, uint32_t len);

// CRC a region of flash by absolute address, no copy.
uint32_t crc32_flash(uint32_t start_addr, uint32_t len);

// CRC `data_size` bytes starting at `start_addr` while copying them into `data`.
// Used by the SMBus page-readback path (WRITE_READ_PAGE, both images' smbus_i2c.c).
uint32_t crc32_copy(uint32_t start_addr, uint8_t* data, uint16_t data_size);
