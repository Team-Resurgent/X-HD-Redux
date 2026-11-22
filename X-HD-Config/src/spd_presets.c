// SPDX-License-Identifier: GPL-3.0-only
#include "spd_presets.h"
#include <stdio.h>

static const struct {
    const char *vendor;
    const char *product;
} kPresets[SPD_PRESET_COUNT] = {
    { "MSFT",     "Xbox" },
    { "MSFT",     "Microsoft Xbox" },
    { "MSFT",     "X-HD Redux" },
    { "MSFT",     "Retro Consoles" },
    { "HALO",     "Combat Evolved" },
    { "SEGA",     "Dreamcast" },
    { "SONY",     "PlayStation" },
    { "NINTENDO", "GameCube" }
};

const char *SpdPreset_Vendor(int index)  { return kPresets[index].vendor; }
const char *SpdPreset_Product(int index) { return kPresets[index].product; }

void SpdPreset_Name(int index, char out[25])
{
    snprintf(out, 25, "%-8.8s%-16.16s", kPresets[index].vendor, kPresets[index].product);
}
