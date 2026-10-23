#ifndef __VIDEO_SUPERVISOR_H__
#define __VIDEO_SUPERVISOR_H__

#include <stdbool.h>
#include "../shared/adv7511.h"
#include "../shared/user_config.h"

#ifdef XHD_HOST_TEST
// Host tests only: resets the loop state to its boot values between sessions.
void video_supervisor_init(void);
#endif
// One main-loop iteration (see video_supervisor.c). xb_encoder / encoder_mismatch are the
// caller's boot-time values, updated in place as the bus probe or BIOS packets resolve them.
void video_supervisor_step(adv7511 *enc, xbox_encoder *xb_encoder, bool *encoder_mismatch);

#endif
