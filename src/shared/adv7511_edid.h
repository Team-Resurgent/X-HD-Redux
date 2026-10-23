#ifndef __ADV7511_EDID_H__
#define __ADV7511_EDID_H__

#include <stdint.h>
#include <stdbool.h>

void edid_update_cache(void);
// Non-blocking except for the rare call that actually retries -- see its own comment in
// adv7511_edid.c. Call once per main-loop iteration; true return means a retry just
// succeeded and the caller should treat it like a fresh HPD reconnect (HDMI mode/audio
// restore).
bool edid_retry_periodic(void);
// True while a just-failed EDID read may still succeed (the sink's DDC coming up after its
// HPD): output stays off so the first apply goes out with the sink's real configuration.
bool edid_output_hold(void);
bool edid_is_hdmi_sink(void);
// CEC physical address from the cached EDID's HDMI VSDB (no read); 0xFFFF = unknown.
uint16_t edid_cec_physical_address(void);
bool edid_supports_yuv444(void);
bool edid_supports_underscan(void);
const uint8_t* edid_get_raw_buffer(void);

#endif // __ADV7511_EDID_H__
