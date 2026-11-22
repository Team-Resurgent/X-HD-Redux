// SPDX-License-Identifier: GPL-3.0-only
#include "fw_update.h"
#include "defines.h"
#include "smbus_hal.h"
#include <stdio.h>
#include <string.h>

#define FW_DIR          "D:\\"
#define SMC_ADDR        0x20    // PIC16L system management controller
#define SMC_CMD_POWER   0x02
#define SMC_POWER_CYCLE 0x40

static CRITICAL_SECTION s_lock;

static fw_file_t s_files[FW_MAX_FILES];
static int s_file_count;
static int s_selected = -1;
static uint8_t s_data[FW_MAX_IMAGE];      // the selected image
static fw_image_t s_image;
static bool s_image_ok;
static uint8_t s_scratch[FW_MAX_IMAGE];   // listing checks, then backups
static fw_board_info_t s_info;

// Shared with the worker (s_lock).
static volatile bool s_busy;
static fw_job_t s_job;
static fw_job_t s_finished;
static bool s_finished_ok;
static uint32_t s_done, s_total;
static bool s_app_written;   // the last update rewrote the application (the board restarted)
static char s_log[FW_LOG_LINES][FW_LOG_WIDTH];
static int s_log_count;

// ---------------------------------------------------------------------------- fw_io_t

static void io_sleep(uint32_t ms) { if (ms) Sleep(ms); }
static uint32_t io_ticks(void) { return GetTickCount(); }

static void io_progress(uint32_t done, uint32_t total)
{
    EnterCriticalSection(&s_lock);
    s_done = done;
    s_total = total;
    LeaveCriticalSection(&s_lock);
}

void FwUpdate_Log(const char *msg)
{
    EnterCriticalSection(&s_lock);
    if (s_log_count == FW_LOG_LINES) {
        memmove(s_log[0], s_log[1], sizeof(s_log[0]) * (FW_LOG_LINES - 1));
        s_log_count--;
    }
    snprintf(s_log[s_log_count++], FW_LOG_WIDTH, "%s", msg);
    LeaveCriticalSection(&s_lock);
}

static const fw_io_t kIo = { smbus_read_at, smbus_write_at, io_sleep, io_ticks, io_progress, FwUpdate_Log };

// ---------------------------------------------------------------------------- files

static bool read_file(const char *name, uint8_t *buf, uint32_t max, uint32_t *size)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), FW_DIR "%s", name);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = GetFileSize(h, NULL), got = 0;
    *size = n;
    bool ok = n <= max && ReadFile(h, buf, n, &got, NULL) && got == n;
    CloseHandle(h);
    return ok || n > max;   // too large is reported by fw_image_check
}

static void list_files(void)
{
    s_file_count = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(FW_DIR "*.bin", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || s_file_count == FW_MAX_FILES) continue;
        fw_file_t *f = &s_files[s_file_count];
        memset(f, 0, sizeof(*f));
        snprintf(f->name, sizeof(f->name), "%s", fd.cFileName);
        if (!read_file(f->name, s_scratch, sizeof(s_scratch), &f->size)) {
            f->error = "Could not read the file";
        } else {
            fw_image_t img;
            f->error = fw_image_check(s_scratch, f->size > sizeof(s_scratch) ? sizeof(s_scratch) + 1 : f->size, &img);
            f->has_about = !f->error && img.has_about;
            if (f->has_about) {
                memcpy(f->git, img.git, sizeof(f->git));
                memcpy(f->built, img.built, sizeof(f->built));
            }
        }
        s_file_count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    // By name; firmware.bin first, as the file a release ships.
    for (int i = 1; i < s_file_count; i++) {
        for (int j = i; j > 0; j--) {
            const char *a = s_files[j - 1].name, *b = s_files[j].name;
            bool swap = _stricmp(b, "firmware.bin") == 0 ||
                        (_stricmp(a, "firmware.bin") != 0 && _stricmp(a, b) > 0);
            if (!swap) break;
            fw_file_t t = s_files[j - 1];
            s_files[j - 1] = s_files[j];
            s_files[j] = t;
        }
    }
}

// The settings layout tag the running application saves (xhd_user_config.magic).
static bool read_cfg_magic(uint32_t *magic)
{
    uint8_t b[4];
    if (!smbus_write_byte(XHD_OP_WRITE_USER_CONFIG_INDEX, 0)) return false;
    for (int i = 0; i < 4; i++) {
        if (!smbus_read_byte(XHD_OP_READ_USER_CONFIG, &b[i])) return false;
    }
    *magic = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return *magic != 0x42424242u && *magic != 0xFFFFFFFFu;
}

static void read_board(void)
{
    memset(&s_info, 0, sizeof(s_info));
    smbus_detect(&s_info.board);
    if (s_info.board.kind == FW_BOARD_XHD_APP) {
        s_info.board_build = fw_read_build(&kIo, s_info.git, s_info.built);
        s_info.have_cfg_magic = read_cfg_magic(&s_info.cfg_magic);
    }
}

// ---------------------------------------------------------------------------- API

void FwUpdate_Init(void)
{
    InitializeCriticalSection(&s_lock);
}

void FwUpdate_Rescan(void)
{
    if (s_busy) return;
    read_board();
    char keep[48] = "";
    if (s_selected >= 0) snprintf(keep, sizeof(keep), "%s", s_files[s_selected].name);
    list_files();
    s_selected = -1;
    s_image_ok = false;
    int pick = 0;
    for (int i = 0; i < s_file_count; i++) {
        if (keep[0] && _stricmp(s_files[i].name, keep) == 0) pick = i;
    }
    if (s_file_count > 0) FwUpdate_Select(pick);
}

int FwUpdate_FileCount(void) { return s_file_count; }
const fw_file_t *FwUpdate_File(int index) { return index >= 0 && index < s_file_count ? &s_files[index] : NULL; }
int FwUpdate_Selected(void) { return s_selected; }
const fw_image_t *FwUpdate_Image(void) { return s_image_ok ? &s_image : NULL; }
const fw_board_info_t *FwUpdate_Board(void) { return &s_info; }

bool FwUpdate_Select(int index)
{
    if (s_busy || index < 0 || index >= s_file_count) return false;
    s_selected = index;
    s_image_ok = false;
    s_info.compared = false;
    uint32_t size = 0;
    if (s_files[index].error || !read_file(s_files[index].name, s_data, sizeof(s_data), &size)) {
        return false;
    }
    s_image_ok = fw_image_check(s_data, size, &s_image) == NULL;
    return s_image_ok;
}

static bool save_backup(void)
{
    char name[48], path[MAX_PATH];
    for (int n = 0; n < 100; n++) {
        const char *tag = s_info.board_build ? s_info.git : "unknown";
        if (n == 0) snprintf(name, sizeof(name), "backup_%.8s.bin", tag);
        else snprintf(name, sizeof(name), "backup_%.8s_%d.bin", tag, n);
        snprintf(path, sizeof(path), FW_DIR "%s", name);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) break;
    }
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        FwUpdate_Log("Could not create the backup file (read-only drive?)");
        return false;
    }
    DWORD put = 0;
    bool ok = WriteFile(h, s_scratch, FW_BACKUP_SIZE, &put, NULL) && put == FW_BACKUP_SIZE;
    CloseHandle(h);
    char msg[FW_LOG_WIDTH];
    snprintf(msg, sizeof(msg), ok ? "Saved %s next to the XBE (flashable, settings page not included)" : "Writing %s failed", name);
    FwUpdate_Log(msg);
    return ok;
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    bool ok = false;
    switch (s_job) {
    case FW_JOB_COMPARE: {
        fw_compare_t c;
        ok = fw_compare(&kIo, &s_image, &c);
        EnterCriticalSection(&s_lock);
        s_info.compare = c;
        s_info.compared = ok;
        LeaveCriticalSection(&s_lock);
        break;
    }
    case FW_JOB_UPDATE:
        ok = fw_update_firmware(&kIo, &s_image, &s_app_written);
        break;
    case FW_JOB_CONVERT:
        ok = fw_convert_hdplus(&kIo, &s_image);
        break;
    case FW_JOB_BACKUP:
        ok = fw_read_board(&kIo, s_scratch) && save_backup();
        break;
    default:
        break;
    }
    // Also when only the bootloader failed: the new application is running.
    if ((ok && s_job == FW_JOB_CONVERT) || (s_job == FW_JOB_UPDATE && s_app_written)) {
        char git[24], built[20], msg[FW_LOG_WIDTH];
        if (fw_read_build(&kIo, git, built)) {
            bool match = !s_image.has_about || strcmp(git, s_image.git) == 0;
            snprintf(msg, sizeof(msg), "%s build %s (%s)", match ? "Running" : "WARNING: running", git, built);
            FwUpdate_Log(msg);
        }
    }
    EnterCriticalSection(&s_lock);
    s_finished = s_job;
    s_finished_ok = ok;
    s_job = FW_JOB_NONE;
    s_busy = false;
    LeaveCriticalSection(&s_lock);
    return 0;
}

bool FwUpdate_Start(fw_job_t job)
{
    if (s_busy) return false;
    if (job != FW_JOB_BACKUP && !s_image_ok) return false;
    EnterCriticalSection(&s_lock);
    s_job = job;
    s_busy = true;
    s_done = s_total = 0;
    if (job != FW_JOB_COMPARE) s_log_count = 0;
    LeaveCriticalSection(&s_lock);
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (t == NULL) {
        s_busy = false;
        s_job = FW_JOB_NONE;
        FwUpdate_Log("Could not start the worker thread");
        return false;
    }
    CloseHandle(t);
    return true;
}

bool FwUpdate_IsBusy(void) { return s_busy; }
bool FwUpdate_AppWritten(void) { return s_app_written; }

fw_job_t FwUpdate_TakeFinished(bool *ok)
{
    EnterCriticalSection(&s_lock);
    fw_job_t j = s_finished;
    *ok = s_finished_ok;
    s_finished = FW_JOB_NONE;
    LeaveCriticalSection(&s_lock);
    return j;
}

void FwUpdate_GetProgress(fw_job_t *job, uint32_t *done, uint32_t *total)
{
    EnterCriticalSection(&s_lock);
    *job = s_job;
    *done = s_done;
    *total = s_total;
    LeaveCriticalSection(&s_lock);
}

int FwUpdate_GetLog(char lines[FW_LOG_LINES][FW_LOG_WIDTH])
{
    EnterCriticalSection(&s_lock);
    int n = s_log_count;
    memcpy(lines, s_log, sizeof(s_log));
    LeaveCriticalSection(&s_lock);
    return n;
}

void FwUpdate_RebootConsole(void)
{
    smbus_write_at(SMC_ADDR, SMC_CMD_POWER, SMC_POWER_CYCLE);
    for (;;) Sleep(1000);
}
