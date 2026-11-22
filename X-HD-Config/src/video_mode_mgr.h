// SPDX-License-Identifier: GPL-3.0-only
#ifndef VIDEO_MODE_MGR_H
#define VIDEO_MODE_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "xbox_base.h"
#include <d3d8.h>

#define MAX_VIDEO_MODES 8

typedef struct {
    char     name[32];          // e.g. "480p (720x480) @ 60Hz"
    uint16_t width;             // Raster width (720, 1280, 1920)
    uint16_t height;            // Raster height (480, 576, 720, 1080)
    uint16_t refresh_rate;      // Refresh rate in Hz (60, 50 for 576i)
    bool     interlaced;        // true = interlaced ('i'), false = progressive ('p')
    bool     widescreen;        // true = 16:9, false = 4:3
    DWORD    d3d_flags;         // D3DPRESENTFLAG_*
    UINT     adapter_mode_idx;  // Underlying D3D adapter mode index
} video_mode_info_t;

void VideoMode_Init(void);
int  VideoMode_GetCount(void);
const video_mode_info_t* VideoMode_Get(int index);
int  VideoMode_GetCurrentIndex(void);
void VideoMode_SetCurrentIndex(int index);

#endif // VIDEO_MODE_MGR_H
