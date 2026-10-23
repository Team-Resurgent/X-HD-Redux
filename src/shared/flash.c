#include "flash.h"

#include <string.h>

#include "defines.h"
#include "stm32.h"
#include "crc32.h"

static uint32_t flash_page_addr(uint16_t page)
{
    return FLASH_START_ADDRESS + ((uint32_t)page << FLASH_PAGE_SHIFT);
}

bool flash_erase_page(uint16_t page)
{
    uint32_t flash_addr = flash_page_addr(page);

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef eraseInitStruct;
    eraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
    eraseInitStruct.PageAddress = flash_addr;
    eraseInitStruct.NbPages = 1;

    uint32_t pageError = 0;
    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&eraseInitStruct, &pageError);

    HAL_FLASH_Lock();

    return status == HAL_OK;
}

bool flash_write_page(uint16_t page, uint8_t* data, uint16_t data_size)
{
    uint32_t flash_addr = flash_page_addr(page);

    HAL_FLASH_Unlock();

    HAL_StatusTypeDef status = HAL_OK;
    for (uint32_t i = 0; i < data_size; i += 2)
    {
        uint16_t half_word = data[i] | (data[i + 1] << 8);
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr + i, half_word);
        if (status != HAL_OK)
        {
            break;
        }
    }

    HAL_FLASH_Lock();

    return status == HAL_OK;
}

uint32_t flash_copy_page(uint16_t page, uint8_t* data, uint16_t data_size)
{
    uint32_t flash_addr = flash_page_addr(page);
    return crc32_copy(flash_addr, data, data_size);
}

bool flash_program_page_verified(uint16_t page, const uint8_t *buf, uint16_t len, uint32_t expect_crc)
{
    if (!flash_erase_page(page)) {
        return false;
    }
    if (!flash_write_page(page, (uint8_t *)buf, len)) {
        return false;
    }
    uint32_t flash_addr = flash_page_addr(page);
    return crc32_flash(flash_addr, len) == expect_crc;
}

// Page 63 also holds xhd_user_config (application's saved calibration/settings,
// see user_config.h) at the start of the page -- this flag is only its last 2
// bytes. Clearing the flag needs an erase (flash can only program 1->0, and the
// flag's bits must go back to 1), which would otherwise silently wipe that config.
// Preserve/restore everything but the flag halfword across the erase so the two
// don't clobber each other. Bootloader-only (its SMBus/UART updaters are the only callers).
#ifndef BUILD_APPLICATION
void flash_remove_flag()
{
    uint32_t page_addr = flash_page_addr(LAST_FLASH_PAGE);
    static uint8_t s_preserved[FLASH_PAGE_SIZE - 2];

    memcpy(s_preserved, (void *)page_addr, sizeof(s_preserved));

    flash_erase_page(LAST_FLASH_PAGE);

    flash_write_page(LAST_FLASH_PAGE, s_preserved, sizeof(s_preserved));
}
#endif

// No erase needed here: the flag only ever moves 0xFFFF (erased) -> APP_INVALID_FLAG,
// and flash programming can always clear bits from 1 to 0. Erasing first (the old
// behavior) needlessly wiped the rest of page 63 -- see flash_remove_flag() above.
void flash_set_flag()
{
    HAL_FLASH_Unlock();
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, APP_INVALID_FLAG_ADDRESS, APP_INVALID_FLAG);
    HAL_FLASH_Lock();
}
