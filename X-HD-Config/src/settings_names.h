// SPDX-License-Identifier: GPL-3.0-only
// Display names for the X-HD's settings and status codes, in one place for every screen.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XHD_COLOR_SPACE_COUNT 7
#define XHD_ASPECT_COUNT      3
#define XHD_SCAN_COUNT        3
#define XHD_CLOCK_PHASE_COUNT 8

// XHD_COLOR_SPACE_*: the full name for the settings list, the short one for tight rows
// (Black Level Check, the wire readout).
const char *XhdName_ColorSpace(uint8_t cs);
const char *XhdName_ColorSpaceShort(uint8_t cs);
const char *XhdName_Aspect(uint8_t ar);
const char *XhdName_Scan(uint8_t sm);
// READ_TELEMETRY probed_encoder (the encoder's SMBus address byte).
const char *XhdName_Encoder(uint8_t enc);
// READ_TELEMETRY arb_source.
const char *XhdName_ArbSource(uint8_t src);

#ifdef __cplusplus
}
#endif
