#pragma once

// Line number only: the one caller is stm32f0xx_clock_config.c, so __FILE__ would just add a
// 44-byte path string to both images.
void _Error_Handler(int line);
#define Error_Handler() _Error_Handler(__LINE__)
