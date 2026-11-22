// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "xbox_base.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Point D: at the folder this XBE was started from. Not every launcher does (some leave D: on
// the DVD drive), and the app reads its images and Media from, and writes backups to, D:.
bool AppDir_MountD(void);

#ifdef __cplusplus
}
#endif
