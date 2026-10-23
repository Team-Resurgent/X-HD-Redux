#ifndef __XBOX_VIDEO_BIOS_H__
#define __XBOX_VIDEO_BIOS_H__

#include <stdint.h>
#include <stdbool.h>
#include "../shared/types.h"
#include "../shared/xbox_video_timing.h"

typedef enum {
    VIDEO_REGION_NTSCM = 0x00000100,
    VIDEO_REGION_NTSCJ = 0x00000200,
    VIDEO_REGION_PAL_I = 0x00000300,
    VIDEO_REGION_PAL_M = 0x00000400
} video_region;

// Mode Bitfields
#define XBOX_VIDEO_MODE_BIT_WIDESCREEN 0x10000000
#define XBOX_VIDEO_MODE_BIT_SCART      0x20000000 // RGB

// avinfo bit 16: toggles per title on consoles that populate avinfo (e.g. Focus), even where
// mode bit 28 doesn't -- the 480p aspect fallback in bios_resolve_mode().
#define XBOX_VIDEO_AVINFO_BIT_WIDESCREEN 0x00010000

void bios_loop(xbox_encoder * xb_encoder, bool * encoder_mismatch);
uint8_t get_active_bios_mode_index(void);
void reapply_active_bios_mode(const xbox_encoder xb_encoder);

// Applies the pending packet, if any (a no-op otherwise). Call every main-loop pass; the
// arbiter decides whether BIOS owns the picture.
void bios_reconcile_pending_target(const xbox_encoder xb_encoder);

// True while a packet waits to be applied (standalone's blind fallback holds off).
bool bios_reconcile_target_pending(void);

// The last-received mode dword (bit 28 = widescreen, bits 23:16 = mode_index), for the
// arbiter snapshot.
uint32_t bios_last_mode_dword(void);

// Reapplies the active mode through whichever path owns the picture (BIOS or standalone).
// Use this to make a setting take effect -- forcing the standalone path would replace a
// BIOS-controlled mode with an autodetected guess.
void reapply_active_mode(const xbox_encoder xb_encoder);

// Resolves and applies a mode dword now (reapplies, the `m` CLI command). False for an
// unsupported mode_index.
bool set_video_mode_bios(const xbox_encoder xb_encoder, const uint32_t mode, const uint32_t avinfo);
#endif // __XBOX_VIDEO_BIOS_H__
