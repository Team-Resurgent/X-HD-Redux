// SPDX-License-Identifier: GPL-3.0-only
#include "video_mode_mgr.h"
#include <stdio.h>
#include <string.h>

static video_mode_info_t s_modes[MAX_VIDEO_MODES];
static int               s_mode_count = 0;
static int               s_current_index = -1;
static bool              s_initialized = false;

static void add_mode(const char *name, uint16_t w, uint16_t h, uint16_t hz,
                     bool interlaced, bool wide, DWORD flags, UINT adapter_idx)
{
    if (s_mode_count >= MAX_VIDEO_MODES) return;

    // Check for raster duplicate (same width, height, interlace state)
    for (int i = 0; i < s_mode_count; i++) {
        if (s_modes[i].width == w && s_modes[i].height == h && s_modes[i].interlaced == interlaced) {
            return;
        }
    }

    video_mode_info_t *m = &s_modes[s_mode_count];
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->width            = w;
    m->height           = h;
    m->refresh_rate     = hz;
    m->interlaced       = interlaced;
    m->widescreen       = wide;
    m->d3d_flags        = flags;
    m->adapter_mode_idx = adapter_idx;

    s_mode_count++;
}

void VideoMode_Init(void)
{
    if (s_initialized) return;

    s_mode_count = 0;
    s_current_index = 0;

    // Only the modes the dashboard settings allow, best first; the app opens in the first one
    // the console accepts. SD is 720x480 (CEA VIC 2/3 progressive, 6/7 interlaced), never
    // 640x480, with the widescreen flag as the dashboard has it; a PAL console without PAL-60
    // gets 576i instead of 480i.
    const DWORD vf = XGetVideoFlags();
    const DWORD sdWide = (vf & XC_VIDEO_FLAGS_WIDESCREEN) ? D3DPRESENTFLAG_WIDESCREEN : 0;
    // HD modes (480p/720p/1080i) only on NTSC: a PAL-I console keeps the dashboard's HD flags
    // but still outputs 576i, and a 720p back buffer then showed only its top-left corner.
    const DWORD std = XGetVideoStandard();
    const bool hd_ok = std == XC_VIDEO_STANDARD_NTSC_M || std == XC_VIDEO_STANDARD_NTSC_J;
    if (hd_ok && (vf & XC_VIDEO_FLAGS_HDTV_720p)) {
        add_mode("720p @ 60Hz", 1280, 720, 60, false, true, D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN, 0);
    }
    if (hd_ok && (vf & XC_VIDEO_FLAGS_HDTV_1080i)) {
        add_mode("1080i @ 60Hz", 1920, 1080, 60, true, true, D3DPRESENTFLAG_INTERLACED | D3DPRESENTFLAG_WIDESCREEN, 0);
    }
    if (hd_ok && (vf & XC_VIDEO_FLAGS_HDTV_480p)) {
        add_mode("480p (720x480) @ 60Hz", 720, 480, 60, false, sdWide != 0, D3DPRESENTFLAG_PROGRESSIVE | sdWide, 0);
    }
    if (std == XC_VIDEO_STANDARD_PAL_I && !(vf & XC_VIDEO_FLAGS_PAL_60Hz)) {
        add_mode("576i (720x576) @ 50Hz", 720, 576, 50, true, sdWide != 0, D3DPRESENTFLAG_INTERLACED | sdWide, 0);
    } else {
        add_mode("480i (720x480) @ 60Hz", 720, 480, 60, true, sdWide != 0, D3DPRESENTFLAG_INTERLACED | sdWide, 0);
    }

    s_initialized = true;
}

int VideoMode_GetCount(void)
{
    return s_mode_count;
}

const video_mode_info_t* VideoMode_Get(int index)
{
    if (index < 0 || index >= s_mode_count) return NULL;
    return &s_modes[index];
}

int VideoMode_GetCurrentIndex(void)
{
    return s_current_index;
}

void VideoMode_SetCurrentIndex(int index)
{
    if (index >= 0 && index < s_mode_count) {
        s_current_index = index;
    }
}
