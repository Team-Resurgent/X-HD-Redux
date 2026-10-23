// Copyright 2021, Ryan Wendland, XboxHDMI by Ryzee119
// SPDX-License-Identifier: MIT

#pragma once

#include "../shared/error_handler.h"

#include <stdbool.h>

bool can_launch_application(void);
void jump_to_application(void);
void enter_bootloader_mode(void);
void enter_bootloader_mode_fw(void);
