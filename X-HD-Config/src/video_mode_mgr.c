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

    // Rasters to calibrate (index 0 is the launch mode):
    add_mode("720p @ 60Hz",           1280, 720,  60, false, true,  D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN, 0);
    add_mode("1080i @ 60Hz",          1920, 1080, 60, true,  true,  D3DPRESENTFLAG_INTERLACED  | D3DPRESENTFLAG_WIDESCREEN, 0);
    // 720x480: ITU-R BT.601 D1 width
    add_mode("480p (720x480) @ 60Hz", 720,  480,  60, false, true,  D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN, 0);
    add_mode("480p (640x480) @ 60Hz", 640,  480,  60, false, false, D3DPRESENTFLAG_PROGRESSIVE, 0);
    add_mode("480i (640x480) @ 60Hz", 640,  480,  60, true,  false, D3DPRESENTFLAG_INTERLACED,  0);

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

int VideoMode_GetNextIndex(int currentIndex)
{
    if (s_mode_count <= 0) return 0;
    return (currentIndex + 1) % s_mode_count;
}

int VideoMode_GetPrevIndex(int currentIndex)
{
    if (s_mode_count <= 0) return 0;
    return (currentIndex - 1 + s_mode_count) % s_mode_count;
}

int VideoMode_FindMatch(uint16_t width, uint16_t height, bool interlaced)
{
    // Try exact match first (width, height, interlace)
    for (int i = 0; i < s_mode_count; i++) {
        if (s_modes[i].width == width && s_modes[i].height == height && s_modes[i].interlaced == interlaced) {
            return i;
        }
    }
    // Fallback to height and interlace
    for (int i = 0; i < s_mode_count; i++) {
        if (s_modes[i].height == height && s_modes[i].interlaced == interlaced) {
            return i;
        }
    }
    return -1;
}
