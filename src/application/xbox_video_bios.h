// SPDX-License-Identifier: GPL-3.0-only
#ifndef __XBOX_VIDEO_BIOS_H__
#define __XBOX_VIDEO_BIOS_H__

#include <stdint.h>
#include <stdbool.h>
#include "../shared/types.h"
#include "../shared/xbox_video_timing.h"

// Mode dword bit: 16:9 for SD modes (HD is 16:9 by format; the 480p modes never set it).
#define XBOX_VIDEO_MODE_BIT_WIDESCREEN 0x10000000

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

// A setting changed: re-apply the active mode once the picture is settled (no packet
// pending, no standalone decision under way, no TMDS cycle, a stable clock) -- or not at
// all if an apply comes first, since it already uses the setting. Run by bios_loop().
void request_reapply(void);
// Drops a queued request: the console's signal went, and the next picture's apply takes the
// current settings.
void cancel_reapply(void);
// Carries out a request_reapply() once the picture is settled. Run after the pass's mode
// decisions (video_supervisor_step()), so an apply they make counts first.
void service_reapply(const xbox_encoder xb_encoder);

// Resolves and applies a mode dword now (reapplies, the `m` CLI command). False for an
// unsupported mode_index.
bool set_video_mode_bios(const xbox_encoder xb_encoder, const uint32_t mode, const uint32_t avinfo);
#endif // __XBOX_VIDEO_BIOS_H__
