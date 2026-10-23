#include "smbus_slave_common.h"
#include "adv7511_i2c.h"
#include "defines.h"

static I2C_HandleTypeDef hi2c2;
static uint32_t state = SMBUS_SMS_READY;
static int currentCommand = -1;  // -1 means no command, otherwise stores command byte
static uint8_t commandByte = 0;
static uint8_t dataByte = 0;
static uint8_t responseByte = 0x42;  // what a read before any command would get (it is NACKed)

// No stuck-SCL watchdog: the interrupt priorities prevent it (see application/main.c).

void smbus_slave_init(void)
{
    I2C_HandleTypeDef *h = &hi2c2;
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C2_CLK_ENABLE();

    // I2C2 pins: PB10=SCL, PB11=SDA
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_10 | GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_I2C2;
    HAL_GPIO_Init(GPIOB, &gpio);

    h->Instance = I2C2;
    // PCLK1 is 24 MHz (HSI/2 x 6, stm32f0xx_clock_config.c). PRESC = 5 divides it by 6:
    // 250 ns ticks. A slave uses only SCLDEL (3 -> 1 us data setup) and SDADEL (0);
    // SCLL/SCLH are master-only. The value was derived for a 48 MHz clock (125 ns ticks), so
    // the setup time is twice what was intended -- well inside SMBus limits, and what every
    // board has run.
    h->Init.Timing = 0x50303D5B;
    h->Init.OwnAddress1 = (I2C_SLAVE_ADDR << 1);
    h->Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    h->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    h->Init.OwnAddress2 = 0;
    h->Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    h->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    h->Init.NoStretchMode = I2C_NOSTRETCH_DISABLE; // allow clock stretching

    i2c_init_or_hang(h, "SMBUS");

    HAL_NVIC_SetPriority(I2C2_IRQn, 1, 0);   // the highest (map in application/main.c)
    HAL_NVIC_EnableIRQ(I2C2_IRQn);

    state = SMBUS_SMS_READY;
    currentCommand = -1;
    HAL_I2C_EnableListen_IT(h);
}

void smbus_set_mode_and_reset(uint8_t mode)
{
    if (mode == I2C_HDMI_MODE_BOOTLOADER) {
        *BOOTLOADER_FLAG_ADDRESS = BOOTLOADER_MAGIC_VALUE;
    }
    if (mode == I2C_HDMI_MODE_APPLICATION) {
        *BOOTLOADER_FLAG_ADDRESS = 0;
    }
    // Not HAL_Delay(10) -- this runs inside the I2C2 ISR (priority 1), which SysTick
    // (priority 3) can't preempt, so the tick would never advance. A busy-wait lets the
    // write's ACK finish before the reset: 120000 iterations were sized for ~10 ms at 48 MHz;
    // at the real 24 MHz it is ~20 ms.
    for (volatile uint32_t i = 0; i < 120000; i++) { __NOP(); }
    NVIC_SystemReset();
}

// -------------------- Address Match Callback --------------------
void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c, uint8_t TransferDirection, uint16_t AddrMatchCode)
{
    (void)AddrMatchCode;
    if(hi2c->Instance != I2C2) return;

    smbus_slave_on_address();

    if(TransferDirection == I2C_DIRECTION_RECEIVE)
    {
        // Master reads (slave transmits). The response was prepared when the command
        // byte arrived (SlaveRxCpltCallback); no command means NACK.
        if (!(state & (SMBUS_SMS_READY | SMBUS_SMS_RESPONSE_READY)) && currentCommand != -1)
        {
            state |= SMBUS_SMS_RESPONSE_READY;
        }

        if (state & SMBUS_SMS_RESPONSE_READY)
        {
            state |= SMBUS_SMS_TRANSMIT;
            // Disable SBC, cannot NACK on TX
            LL_I2C_DisableSlaveByteControl(hi2c->Instance);
            HAL_I2C_Slave_Seq_Transmit_IT(hi2c, &responseByte, 1, I2C_LAST_FRAME);
        }
        else
        {
            __HAL_I2C_GENERATE_NACK(hi2c); // NACK if no command
            LL_I2C_ClearFlag_ADDR(hi2c->Instance);
            HAL_I2C_EnableListen_IT(hi2c);
        }
    }
    else
    {
        // Master writes (slave receives) - new command
        if (state & SMBUS_SMS_READY)
        {
            state &= ~SMBUS_SMS_READY;
            state |= SMBUS_SMS_RECEIVE;
            currentCommand = -1;
            // Enable SBC so we can NACK
            LL_I2C_EnableSlaveByteControl(hi2c->Instance);
            // NEXT_FRAME: a write command's data byte follows in the same transfer
            HAL_I2C_Slave_Seq_Receive_IT(hi2c, &commandByte, 1, I2C_NEXT_FRAME);
        }
    }
}

// -------------------- Receive Complete Callback --------------------
void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if(hi2c->Instance != I2C2) return;

    if (currentCommand == -1)
    {
        // Command byte received: prepare the read response now, while SCL is stretched.
        state &= ~SMBUS_SMS_RECEIVE;
        currentCommand = commandByte;
        responseByte = smbus_slave_read(commandByte, responseByte);

        if ((currentCommand & I2C_WRITE_BIT) == I2C_WRITE_BIT)
        {
            // Write command - receive data byte
            state |= SMBUS_SMS_RECEIVE;
            HAL_I2C_Slave_Seq_Receive_IT(hi2c, &dataByte, 1, I2C_LAST_FRAME);
        }
        else
        {
            // Release the SCL stretch
            LL_I2C_SetTransferSize(hi2c->Instance, 1);
        }
    }
    else
    {
        // Data byte received
        state &= ~SMBUS_SMS_RECEIVE;
    }
}

// -------------------- Transmit Complete Callback --------------------
void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if(hi2c->Instance != I2C2) return;
    state &= ~SMBUS_SMS_TRANSMIT;
}

// -------------------- Listen Complete Callback --------------------
void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if(hi2c->Instance != I2C2) return;

    // Process the write if we have a write command and all data was received
    // (for a single-byte write, XferCount is then 0).
    if(currentCommand != -1 && !(state & SMBUS_SMS_RESPONSE_READY) && hi2c->XferCount == 0)
    {
        smbus_slave_write((uint8_t)currentCommand, dataByte);
    }

    // Reset state, listen again
    state = SMBUS_SMS_READY;
    currentCommand = -1;
    HAL_I2C_EnableListen_IT(hi2c);
}

// -------------------- Error Callback --------------------
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if(hi2c->Instance != I2C2) return;

    uint32_t err = hi2c->ErrorCode;

    if (err & (HAL_I2C_ERROR_BERR | HAL_I2C_ERROR_TIMEOUT))
    {
        // Critical error - reset the stack
        if(state & (SMBUS_SMS_TRANSMIT | SMBUS_SMS_RECEIVE))
        {
            __HAL_I2C_DISABLE(hi2c);
            while (LL_I2C_IsEnabled(hi2c->Instance)) {}
            __HAL_I2C_ENABLE(hi2c);
            if(hi2c->State != HAL_I2C_STATE_READY)
            {
                HAL_I2C_DeInit(hi2c);
                HAL_I2C_Init(hi2c);
                HAL_I2C_EnableListen_IT(hi2c);
            }
        }
        state = SMBUS_SMS_READY;
        currentCommand = -1;
        return;
    }
    if ((err & HAL_I2C_ERROR_AF) && !(err & HAL_I2C_ERROR_ARLO))
    {
        // NACK - expected at end of read, handle gracefully
        hi2c->PreviousState = hi2c->State;
        hi2c->State = HAL_I2C_STATE_READY;
        __HAL_UNLOCK(hi2c);
    }
    if (err != HAL_I2C_ERROR_NONE)
    {
        // NACK, arbitration lost or anything else: drop the transaction, listen again
        state = SMBUS_SMS_READY;
        currentCommand = -1;
        HAL_I2C_EnableListen_IT(hi2c);
    }
}

// -------------------- IRQ Handler --------------------
void I2C2_IRQHandler(void)
{
    if (hi2c2.Instance->ISR & (I2C_FLAG_BERR | I2C_FLAG_ARLO | I2C_FLAG_OVR | I2C_FLAG_TIMEOUT | I2C_FLAG_ALERT | I2C_FLAG_PECERR))
    {
        HAL_I2C_ER_IRQHandler(&hi2c2);
    }
    else
    {
        HAL_I2C_EV_IRQHandler(&hi2c2);
    }
}
