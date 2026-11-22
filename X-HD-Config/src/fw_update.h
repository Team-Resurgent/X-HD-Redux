// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Runs fw_flash.c's jobs on a worker thread so the screen keeps drawing, and handles the
// files: firmware images are the *.bin files in the XBE's folder (D:, see app_dir.c), and
// backups are written there too.

#include "xbox_base.h"
#include "fw_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FW_MAX_FILES   16
#define FW_LOG_LINES   8
#define FW_LOG_WIDTH   96

typedef enum {
    FW_JOB_NONE = 0,
    FW_JOB_COMPARE,
    FW_JOB_UPDATE,
    FW_JOB_CONVERT,
    FW_JOB_BACKUP,
} fw_job_t;

typedef struct {
    char       name[48];
    uint32_t   size;
    const char *error;     // NULL: a usable image
    bool       has_about;
    char       git[24];
    char       built[20];
} fw_file_t;

typedef struct {
    fw_board_t board;
    bool       board_build;    // git/built below are the running application's
    char       git[24];
    char       built[20];
    bool       have_cfg_magic; // the settings layout the running application saves
    uint32_t   cfg_magic;
    bool       compared;       // compare holds the selected image vs the board
    fw_compare_t compare;
} fw_board_info_t;

void FwUpdate_Init(void);
// Detect the board and list the images. Not while a job runs.
void FwUpdate_Rescan(void);
int  FwUpdate_FileCount(void);
const fw_file_t *FwUpdate_File(int index);
int  FwUpdate_Selected(void);
// Load file `index` as the image to flash; false if it isn't usable.
bool FwUpdate_Select(int index);
const fw_image_t *FwUpdate_Image(void);        // NULL with nothing usable selected
const fw_board_info_t *FwUpdate_Board(void);

bool FwUpdate_Start(fw_job_t job);
bool FwUpdate_IsBusy(void);
// The last FW_JOB_UPDATE rewrote the application (so the console needs its power cycle).
bool FwUpdate_AppWritten(void);
// The job that just ended and whether it worked; FW_JOB_NONE once taken.
fw_job_t FwUpdate_TakeFinished(bool *ok);
void FwUpdate_GetProgress(fw_job_t *job, uint32_t *done, uint32_t *total);
int  FwUpdate_GetLog(char lines[FW_LOG_LINES][FW_LOG_WIDTH]);
void FwUpdate_Log(const char *msg);

// Full power cycle through the SMC: the X-HD must see the console boot to capture the EEPROM.
void FwUpdate_RebootConsole(void);

#ifdef __cplusplus
}
#endif
