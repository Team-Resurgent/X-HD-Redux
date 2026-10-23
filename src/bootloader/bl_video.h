#ifndef __BL_VIDEO_H__
#define __BL_VIDEO_H__

// Minimal cold-start video for the bootloader's "waiting for recovery" mode
// (enter_bootloader_mode). Follows the ADV7511 line detector (0x3E): a known VIC
// is applied with fixed, encoder-agnostic Xcalibur-column timing; a blind reading
// falls back to VIC 1 (640x480 SD). No per-encoder tables, no family model, no
// PAL / width / Path-A logic -- that all lives in the application build's
// stand_alone_loop() / vic_arbiter. Call once per main-loop iteration.
void bl_video_follow(void);

#endif // __BL_VIDEO_H__
