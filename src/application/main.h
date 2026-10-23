// Copyright 2021, Ryan Wendland, XboxHDMI by Ryzee119
// SPDX-License-Identifier: MIT

#pragma once

#include "../shared/error_handler.h"

// Follows user_config cec_clock_off (see main.c); called once per supervisor pass.
void cec_clock_out_follow_config(void);
