// SPDX-License-Identifier: GPL-3.0-only
#include "i2c1.h"
#include "stm32.h"

// The transfers HAL_I2C_Mem_Read/Write made, on the registers (RM0360 26.4.8 master
// transmitter, 26.4.9 master receiver): 7-bit address, 8-bit register address, at most 254
// bytes -- none of the HAL's 10-bit, 16-bit-register or >255-byte reload paths. Register
// access goes through I2C1_RD/I2C1_WR so the host tests can run these against a model of the
// peripheral.
#ifndef I2C1_RD
#define I2C1_RD(r)      (I2C1->r)
#define I2C1_WR(r, v)   (I2C1->r = (v))
// The flag and field positions the host model is written against (RM0360 26.7).
_Static_assert(I2C_ISR_TXE == 1u << 0 && I2C_ISR_TXIS == 1u << 1 && I2C_ISR_RXNE == 1u << 2 &&
               I2C_ISR_NACKF == 1u << 4 && I2C_ISR_STOPF == 1u << 5 && I2C_ISR_TC == 1u << 6 &&
               I2C_ISR_BERR == 1u << 8 && I2C_ISR_ARLO == 1u << 9 && I2C_ISR_BUSY == 1u << 15 &&
               I2C_CR2_RD_WRN == 1u << 10 && I2C_CR2_START == 1u << 13 && I2C_CR2_NBYTES_Pos == 16 &&
               I2C_CR2_AUTOEND == 1u << 25 && I2C_ICR_NACKCF == 1u << 4 && I2C_ICR_STOPCF == 1u << 5 &&
               I2C_ICR_BERRCF == 1u << 8 && I2C_ICR_ARLOCF == 1u << 9, "RM0360 I2C bit positions");
#endif

// Waits for `flag`; false on NACK, bus error, arbitration loss or the deadline.
static bool i2c1_wait(const uint32_t flag, const uint32_t t0, const uint32_t timeout_ms)
{
    for (;;) {
        const uint32_t isr = I2C1_RD(ISR);
        if (isr & flag) return true;
        if ((isr & (I2C_ISR_NACKF | I2C_ISR_BERR | I2C_ISR_ARLO)) || HAL_GetTick() - t0 >= timeout_ms) {
            return false;
        }
    }
}

// A failed transfer: after a NACK the peripheral sends the STOP itself (wait for it, briefly);
// then clear the flags, flush TXDR and drop CR2's transfer setup, as the HAL's error path does.
static bool i2c1_fail(void)
{
    if (I2C1_RD(ISR) & I2C_ISR_NACKF) {
        const uint32_t t0 = HAL_GetTick();
        while (!(I2C1_RD(ISR) & I2C_ISR_STOPF) && HAL_GetTick() - t0 < 2u) {}
    }
    I2C1_WR(ICR, I2C_ICR_NACKCF | I2C_ICR_STOPCF | I2C_ICR_BERRCF | I2C_ICR_ARLOCF);
    I2C1_WR(ISR, I2C_ISR_TXE);
    I2C1_WR(CR2, 0);
    return false;
}

// A transfer's end: AUTOEND sent the STOP.
static bool i2c1_done(const uint32_t t0, const uint32_t timeout_ms)
{
    if (!i2c1_wait(I2C_ISR_STOPF, t0, timeout_ms)) return i2c1_fail();
    I2C1_WR(ICR, I2C_ICR_STOPCF);
    I2C1_WR(CR2, 0);
    return true;
}

static bool i2c1_idle(const uint32_t t0, const uint32_t timeout_ms)
{
    while (I2C1_RD(ISR) & I2C_ISR_BUSY) {
        if (HAL_GetTick() - t0 >= timeout_ms) return false;
    }
    return true;
}

bool i2c1_write(uint8_t dev, uint8_t reg, const uint8_t *buf, uint8_t len, uint32_t timeout_ms)
{
    const uint32_t t0 = HAL_GetTick();
    if (!i2c1_idle(t0, timeout_ms)) return false;
    I2C1_WR(CR2, dev | ((uint32_t)(len + 1u) << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START);
    uint8_t next = reg;
    for (uint8_t i = 0; i <= len; i++) {
        if (!i2c1_wait(I2C_ISR_TXIS, t0, timeout_ms)) return i2c1_fail();
        I2C1_WR(TXDR, next);
        if (i < len) next = buf[i];
    }
    return i2c1_done(t0, timeout_ms);
}

bool i2c1_read(uint8_t dev, uint8_t reg, uint8_t *buf, uint8_t len, uint32_t timeout_ms)
{
    const uint32_t t0 = HAL_GetTick();
    if (!i2c1_idle(t0, timeout_ms)) return false;
    // The register address, then a repeated START (no STOP between: TC, not AUTOEND).
    I2C1_WR(CR2, dev | (1u << I2C_CR2_NBYTES_Pos) | I2C_CR2_START);
    if (!i2c1_wait(I2C_ISR_TXIS, t0, timeout_ms)) return i2c1_fail();
    I2C1_WR(TXDR, reg);
    if (!i2c1_wait(I2C_ISR_TC, t0, timeout_ms)) return i2c1_fail();
    I2C1_WR(CR2, dev | I2C_CR2_RD_WRN | ((uint32_t)len << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START);
    for (uint8_t i = 0; i < len; i++) {
        if (!i2c1_wait(I2C_ISR_RXNE, t0, timeout_ms)) return i2c1_fail();
        buf[i] = (uint8_t)I2C1_RD(RXDR);
    }
    return i2c1_done(t0, timeout_ms);
}
