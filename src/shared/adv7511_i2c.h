#ifndef __ADV7511_I2C_H__
#define __ADV7511_I2C_H__

#include "stm32.h"

void adv7511_i2c_init();
// HAL_I2C_Init + analog filter on + digital filter off, hanging with "<name> I2C init
// failed" on any step -- the shared bring-up for I2C1 (ADV7511) and I2C2 (SMBus slave) in
// both images.
void i2c_init_or_hang(I2C_HandleTypeDef *h, const char *name);
I2C_HandleTypeDef* adv7511_i2c_instance();

#endif // __ADV7511_I2C_H__
