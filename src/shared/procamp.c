// SPDX-License-Identifier: GPL-3.0-only
#include "procamp.h"

// x / 2^n, rounded (half up; the arithmetic shift floors negatives).
static int32_t shr_round(int32_t x, uint8_t n) {
    return (x + (1 << (n - 1))) >> n;
}

// Each output row is out = (Cr*c0 + Y*c1 + Cb*c2) / 4096 + c3, scaled by 2^m (programming guide
// Rev G 4.3.8.1, Table 38 port mapping), with 12-bit inputs (8-bit code x 16). The proc-amp
// acts on the input first: Y' = 256 + k_c*(Y - 256), and (Cb, Cr) scaled and turned around
// 2048 (below). So the Y column scales by k_c, the chroma columns mix, and the offset takes back
// what the new columns no longer add at black (Y = 256) and at neutral chroma (2048): (c - c')*256/4096 and
// (c - c')*2048/4096. A coefficient that no longer fits 13 bits moves the table up a scale
// (x2 -> x4, everything halved), as the CSC Mode bits are meant for (Table 37); past x4 it
// saturates. Brightness and a row's bias add b codes = b*16 in 12-bit, / 2^m in table units.
// The scale is x1/x2/x4 for 00/01/1x (ADI's full-range tables use 11 as x4).
void procamp_fold(const uint8_t *table, const procamp_settings *p, uint8_t *out, bool ycbcr) {
    const int32_t kc = ((int32_t)p->contrast * 10486) >> 8;   // Q12 (x 4096/100), exactly 4096 at 100 %
    int32_t c[12];
    int32_t big = 0;
    for (uint8_t i = 0; i < 12; i++) {
        c[i] = ((((int32_t)(table[2 * i] & 0x1F) << 8) | table[2 * i + 1]) ^ 0x1000) - 0x1000;
    }
    // Hue turns (Cb, Cr) counter-clockwise around neutral: Cb' = a Cb - b Cr, Cr' = b Cb + a Cr
    // (a = chroma_cos, b = chroma_sin; +hue takes red toward yellow). Folded, a row's Cr column
    // becomes a c_Cr - b c_Cb and its Cb column a c_Cb + b c_Cr.
    for (uint8_t r = 0; r < 12; r += 4) {
        const int32_t cr = c[r], y = c[r + 1], cb = c[r + 2];
        c[r] = shr_round(cr * p->chroma_cos - cb * p->chroma_sin, 12);
        c[r + 1] = shr_round(y * kc, 12);
        c[r + 2] = shr_round(cb * p->chroma_cos + cr * p->chroma_sin, 12);
        // In 1/16 table steps: Y's at 256, the chroma columns' at 2048 (<< 3).
        c[r + 3] += shr_round(y - c[r + 1] + ((cr + cb - c[r] - c[r + 2]) << 3), 4);
        // The row's gain, around the row's output at that same black and neutral chroma: every
        // coefficient scales, and the offset takes back (1 - g) of what they add there
        // (k16, in 1/16 table steps).
        const int32_t g = ((int32_t)p->gain[r >> 2] * 10486) >> 8;
        const int32_t k16 = ((c[r] + c[r + 2]) << 3) + c[r + 1];
        for (uint8_t i = 0; i < 3; i++) c[r + i] = shr_round(c[r + i] * g, 12);
        c[r + 3] += shr_round((4096 - g) * k16, 16);
    }
    for (uint8_t i = 0; i < 12; i++) {
        const int32_t a = c[i] < 0 ? ~c[i] : c[i];
        if (a > big) big = a;
    }
    uint8_t m = (uint8_t)((table[0] >> 5) & 3);
    while (big > 4095 && m < 2) {
        m++;
        big >>= 1;
        for (uint8_t i = 0; i < 12; i++) c[i] = shr_round(c[i], 1);
    }
    for (uint8_t i = 0; i < 12; i++) {
        int32_t v = c[i];
        // Brightness lifts every RGB row, but of a YCbCr output only Y (row B, offset 7).
        if ((i & 3) == 3) {
            v += ((int32_t)((!ycbcr || i == 7 ? p->brightness : 0) + p->bias[i >> 2]) * 16) >> (m > 2 ? 2 : m);
        }
        v = v > 4095 ? 4095 : v < -4096 ? -4096 : v;
        out[2 * i] = (uint8_t)((table[2 * i] & 0xE0) | ((v >> 8) & 0x1F));
        out[2 * i + 1] = (uint8_t)v;
    }
    out[0] = (uint8_t)((out[0] & 0x9F) | (m << 5));   // the scale bits; unchanged m writes them back
}
