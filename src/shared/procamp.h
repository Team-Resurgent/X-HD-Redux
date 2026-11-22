// SPDX-License-Identifier: GPL-3.0-only
#ifndef __PROCAMP_H__
#define __PROCAMP_H__

#include <stdbool.h>
#include <stdint.h>

// Picture controls (doc/SMBUS.md, READ_PICTURE). Applied only when the CSC runs; the passthrough colour
// spaces get none of it.
#pragma pack(push, 1)
typedef struct {
    uint8_t white_clip;  // output codes 128-255: CEC map 0xC2/C3 = value << 4
    uint8_t black_clip;  // output codes 0-64:    CEC map 0xC0/C1 = value << 4
    uint8_t contrast;    // Y gain in percent (50-150), around Y = 16
    int8_t  brightness;  // output codes added to every channel (-64..64)
    int16_t chroma_cos;  // Q12 saturation x cos(hue), around 128; 4096 = neutral
    int16_t chroma_sin;  // Q12 saturation x sin(hue); + turns red toward yellow
    uint8_t gain[3];     // output rows A, B, C (R, G, B; Cr, Y, Cb) in percent (50-150), each
                         // around its own output at Y = 16, Cb = Cr = 128
    int8_t  bias[3];     // output codes added to rows A, B, C (-32..32)
} procamp_settings;      // 14 bytes
#pragma pack(pop)

// Picture profiles, picked from what goes out: SD on a CEA clock, HD,
// and everything else (a non-CEA clock; wire VIC 0 in honest mode).
#define PROCAMP_PROFILE_SD     0
#define PROCAMP_PROFILE_HD     1
#define PROCAMP_PROFILE_OTHER  2
#define PROCAMP_PROFILES       3

#define PROCAMP_NEUTRAL { 250, 0, 100, 0, 4096, 0, { 100, 100, 100 }, { 0, 0, 0 } }

// out = table (24 bytes, 0x18-0x2F) with contrast, brightness, saturation, hue and the per-row
// gain and bias folded in. ycbcr: the table's output is YCbCr (rows Cr, Y, Cb -- colour mode 6),
// so brightness moves Y alone. Every
// coefficient saturates at 13 bits; the bits sharing a coefficient's high byte (0x18[7:5]
// enable and scale, 0x1A[5] update hold) are kept.
void procamp_fold(const uint8_t *table, const procamp_settings *p, uint8_t *out, bool ycbcr);

#endif // __PROCAMP_H__
