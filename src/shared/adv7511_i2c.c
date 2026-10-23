#include "adv7511_i2c.h"
#include "../shared/debug.h"

static I2C_HandleTypeDef hi2c1;

void adv7511_i2c_init()
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    // Configure PB6 (SCL) and PB7 (SDA) as I2C pins
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF1_I2C1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    hi2c1.Instance = I2C1;
    hi2c1.Init.Timing = 0x00201D2B; // 100kHz @ 8MHz HSI
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    i2c_init_or_hang(&hi2c1, "ADV7511");
}

void i2c_init_or_hang(I2C_HandleTypeDef *h, const char *name)
{
    if (HAL_I2C_Init(h) != HAL_OK ||
        HAL_I2CEx_ConfigAnalogFilter(h, I2C_ANALOGFILTER_ENABLE) != HAL_OK ||
        HAL_I2CEx_ConfigDigitalFilter(h, 0) != HAL_OK) {
        debug_log("%s I2C init failed\r\n", name);
        while (1) {}
    }
}

I2C_HandleTypeDef* adv7511_i2c_instance()
{
    return &hi2c1;
}
