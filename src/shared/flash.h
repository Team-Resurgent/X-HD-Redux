#pragma once

#include <stdbool.h>
#include <stdint.h>

bool flash_erase_page(uint16_t page);
bool flash_write_page(uint16_t page, uint8_t* data, uint16_t data_size);
uint32_t flash_copy_page(uint16_t page, uint8_t* data, uint16_t data_size);

// Erase `page`, write `len` bytes of `buf` to it, then read the page back and
// confirm its CRC-32 matches `expect_crc`. Returns true only on a fully verified
// write. Every page-update path uses it: SMBus WRITE_RAM_APPLY in both images
// (application: bootloader pages; bootloader: application pages) and the
// bootloader's UART updater.
bool flash_program_page_verified(uint16_t page, const uint8_t *buf, uint16_t len, uint32_t expect_crc);

#ifndef BUILD_APPLICATION   // bootloader updaters only
void flash_remove_flag();
#endif
void flash_set_flag();