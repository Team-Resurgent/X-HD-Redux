// SPDX-License-Identifier: GPL-3.0-only
#include "ui_firmware.h"
#include "ui_terminal.h"
#include "fw_update.h"
#include "xbox_eeprom.h"
#include "ui_picklist.h"
#include "confirm.h"
#include "app.h"
#include <stdio.h>
#include <string.h>

typedef enum { IT_FILE, IT_UPDATE, IT_RETRY, IT_CONVERT, IT_BACKUP, IT_REBOOT } item_t;

static item_t s_items[6];
static int s_item_count;
static int s_sel;
static confirm_t s_confirm = CONFIRM_NONE;   // item waiting for its second (A)
static bool s_last_ok;
static fw_job_t s_last_job;
// Set once an update or conversion succeeds; only the reboot clears it. The board restarted
// on its own during the write, so it lost its capture of the console's EEPROM and needs the
// power cycle -- until then it would look broken, so nothing else is offered.
static bool s_reboot_pending;
// A bootloader write that didn't finish rewrote nothing the console needs yet, but the board
// won't start after a power-off until it does: only Retry (the whole update again, which
// redoes what still differs) and a warned reboot are offered.
// Set when a job that failed had already restarted the board, so the retry that works ends
// in the reboot.
static bool s_reboot_after_retry;
static bool bl_incomplete(void)
{
    return fw_bootloader_incomplete() && FwUpdate_Board()->board.kind == FW_BOARD_XHD_APP;
}

static const char *job_name(fw_job_t j)
{
    switch (j) {
    case FW_JOB_COMPARE:    return "Comparing with the board";
    case FW_JOB_UPDATE:     return "Updating the firmware";
    case FW_JOB_CONVERT:    return "Converting HD+ to X-HD";
    case FW_JOB_BACKUP:     return "Reading the board's firmware";
    default:                return "";
    }
}

static void build_items(void)
{
    const fw_board_info_t *b = FwUpdate_Board();
    s_item_count = 0;
    s_items[s_item_count++] = IT_FILE;
    if (bl_incomplete()) {
        s_items[s_item_count++] = IT_RETRY;
        s_items[s_item_count++] = IT_REBOOT;
        s_sel = 1;
        return;
    }
    if (s_reboot_pending) {
        s_items[s_item_count++] = IT_REBOOT;
        s_sel = s_item_count - 1;
        return;
    }
    switch (b->board.kind) {
    case FW_BOARD_XHD_APP:
        s_items[s_item_count++] = IT_UPDATE;
        s_items[s_item_count++] = IT_BACKUP;
        break;
    case FW_BOARD_XHD_BOOT:
        // Waiting in update mode: an update that was cut off, or a new application that didn't
        // start. Writing it again is the only way out (a reboot comes back here), so nothing
        // else is offered.
        s_items[s_item_count++] = IT_UPDATE;
        if (s_sel >= s_item_count) s_sel = s_item_count - 1;
        return;
    case FW_BOARD_HDPLUS:
        s_items[s_item_count++] = IT_CONVERT;
        break;
    default:
        break;
    }
    s_items[s_item_count++] = IT_REBOOT;
    if (s_sel >= s_item_count) s_sel = s_item_count - 1;
}

// With several images nothing is compared until one is picked: a compare reads every page
// back, and the first file in the list may not be the one wanted.
static bool s_picked;
static int  s_file_row = 7;   // where the Image row was drawn, for the pick list

static void compare_if_xhd(void)
{
    fw_board_kind_t k = FwUpdate_Board()->board.kind;
    if ((k == FW_BOARD_XHD_APP || k == FW_BOARD_XHD_BOOT) && FwUpdate_Image() &&
        (s_picked || FwUpdate_FileCount() == 1)) {
        FwUpdate_Start(FW_JOB_COMPARE);
    }
}

static bool s_compare_next;   // compare on the next frame (after main.c's own refresh)

// What the picked image is next to the board's firmware, in one word for the Image block.
// Newer/older come from the about blocks' build stamps (UTC, "YYYY-MM-DDTHH:MM:SS", so they
// sort as text); a build without one ("unknown") is only ever DIFFERENT.
typedef enum { V_NONE, V_SAME, V_NEWER, V_OLDER, V_DIFFERENT } verdict_t;

static bool stamp_ok(const char *s) { return s[0] >= '0' && s[0] <= '9'; }

static verdict_t verdict(void)
{
    const fw_board_info_t *b = FwUpdate_Board();
    const fw_file_t *f = FwUpdate_File(FwUpdate_Selected());
    if (!f || f->error || !FwUpdate_Image() || !b->compared) return V_NONE;
    if (b->compare.app_diff == 0 && b->compare.bl_diff == 0) return V_SAME;
    if (b->board.kind == FW_BOARD_XHD_APP && b->board_build && f->has_about &&
        stamp_ok(f->built) && stamp_ok(b->built) && strcmp(b->git, f->git) != 0) {
        const int c = strcmp(f->built, b->built);
        if (c > 0) return V_NEWER;
        if (c < 0) return V_OLDER;
    }
    return V_DIFFERENT;
}

static void rescan(void)
{
    EEPROM_Refresh(EDID_GetParsed());
    FwUpdate_Rescan();
    build_items();
    compare_if_xhd();
}

void Firmware_Init(void)
{
    FwUpdate_Init();
    EEPROM_Refresh(EDID_GetParsed());
    FwUpdate_Rescan();
    build_items();
}

void Firmware_OnEnter(void)
{
    if (FwUpdate_IsBusy()) return;
    confirm_clear(&s_confirm);
    s_picked = false;
    rescan();
}

void Firmware_PickImage(int index)
{
    confirm_clear(&s_confirm);
    s_picked = true;
    FwUpdate_Select(index);
    build_items();
    compare_if_xhd();
}

bool Firmware_IsBusy(void) { return FwUpdate_IsBusy(); }
// Also while the bootloader is unfinished: the tab stays locked on Retry.
bool Firmware_RebootPending(void) { return s_reboot_pending || bl_incomplete(); }

bool Firmware_WantsFocus(void)
{
    return FwUpdate_Board()->board.kind != FW_BOARD_XHD_APP;
}

bool Firmware_Update(bool *board_changed)
{
    *board_changed = false;
    if (confirm_expired(&s_confirm)) {
        FwUpdate_Log("Cancelled: no second (A) within 10 s");
        return true;
    }
    if (s_compare_next && !FwUpdate_IsBusy()) {
        s_compare_next = false;
        compare_if_xhd();
        return true;
    }
    bool ok;
    fw_job_t done = FwUpdate_TakeFinished(&ok);
    if (done == FW_JOB_NONE) {
        return FwUpdate_IsBusy();   // keep the progress bar moving
    }
    if (done != FW_JOB_COMPARE) {
        s_last_job = done;
        s_last_ok = ok;
    }
    switch (done) {
    case FW_JOB_COMPARE:
        build_items();
        // A confirmed newer build: put the cursor on Update, the likely next press.
        if (verdict() == V_NEWER) {
            for (int i = 0; i < s_item_count; i++) {
                if (s_items[i] == IT_UPDATE) s_sel = i;
            }
        }
        break;
    case FW_JOB_UPDATE:
    case FW_JOB_CONVERT: {
        const bool restarted = done == FW_JOB_CONVERT || FwUpdate_AppWritten() || s_reboot_after_retry;
        s_reboot_after_retry = restarted && !ok;
        if (ok && restarted) {
            s_reboot_pending = true;
            FwUpdate_Log("Done. The console must reboot (power cycle) to finish: press (A).");
        } else if (ok) {
            FwUpdate_Log("Done. The application did not change: no reboot needed.");
        }
        *board_changed = true;
        FwUpdate_Rescan();
        build_items();
        s_compare_next = true;
        break;
    }
    case FW_JOB_BACKUP:
        FwUpdate_Rescan();   // the backup is now one of the files
        build_items();
        break;
    default:
        break;
    }
    return true;
}

// The first (A) asks (`ask`, plus `ask2` if given); a second (A) on the same item starts it.
static void start_confirmed(int item, fw_job_t job, const char *ask, const char *ask2)
{
    if (!confirm_armed(&s_confirm, item)) {
        confirm_arm(&s_confirm, item);
        FwUpdate_Log(ask);
        if (ask2) FwUpdate_Log(ask2);
        return;
    }
    confirm_clear(&s_confirm);
    FwUpdate_Start(job);
}

void Firmware_HandleInput(const input_state_t *in)
{
    if (FwUpdate_IsBusy()) return;
    if (s_reboot_pending && !bl_incomplete()) {
        if (in->a) {
            UI_RunCRTCollapse();
            FwUpdate_RebootConsole();
        }
        return;
    }
    if (in->up && s_sel > 0) { s_sel--; confirm_clear(&s_confirm); }
    if (in->down && s_sel < s_item_count - 1) { s_sel++; confirm_clear(&s_confirm); }
    if (in->b || in->back) {
        if (confirm_any(&s_confirm)) FwUpdate_Log("Cancelled");
        confirm_clear(&s_confirm);
    }
    if (in->y) {
        confirm_clear(&s_confirm);
        rescan();
        return;
    }

    const item_t it = s_items[s_sel];
    if (!in->a) return;

    const bool have_image = FwUpdate_Image() != NULL;
    switch (it) {
    case IT_FILE: {
        static const char *names[FW_MAX_FILES];
        const int n = FwUpdate_FileCount();
        confirm_clear(&s_confirm);
        if (n > 1) {
            for (int i = 0; i < n; i++) names[i] = FwUpdate_File(i)->name;
            Picklist_Open(PICK_ID_FW_IMAGE, "CHOOSE AN IMAGE:", names, n, FwUpdate_Selected(),
                          s_file_row + 1, Terminal_TextWidth("> Image : "));
        } else {
            Firmware_PickImage(FwUpdate_Selected());
        }
        break;
    }
    case IT_UPDATE:
        if (!have_image) { FwUpdate_Log("Choose a usable image first"); break; }
        start_confirmed(s_sel, FW_JOB_UPDATE,
                        "Press (A) again to UPDATE. Keep the console on until it says Done.",
                        // The bootloader writing the pages doesn't follow hot-plug.
                        "Don't switch your display to another input: the picture may not come back until it ends.");
        break;
    case IT_RETRY:
        start_confirmed(s_sel, FW_JOB_UPDATE, "Press (A) again to retry the update.",
                        "If it keeps failing, something outside the board is stopping it: use an ST-Link.");
        break;
    case IT_CONVERT:
        if (!have_image) { FwUpdate_Log("Choose a usable image first"); break; }
        start_confirmed(s_sel, FW_JOB_CONVERT,
                        "Press (A) again to CONVERT (about 6 min). If it fails, recovery needs an ST-Link.",
                        "Don't switch your display to another input: the picture may not come back until it ends.");
        break;
    case IT_BACKUP:
        confirm_clear(&s_confirm);
        FwUpdate_Start(FW_JOB_BACKUP);
        break;
    case IT_REBOOT:
        if (!confirm_armed(&s_confirm, s_sel)) {
            confirm_arm(&s_confirm, s_sel);
            if (bl_incomplete()) {
                FwUpdate_Log("The bootloader isn't written: after a power cycle the board won't start");
                FwUpdate_Log("until it is reflashed with an ST-Link. Press (A) again to reboot anyway.");
            } else {
                FwUpdate_Log("Press (A) again to power-cycle the console.");
            }
        } else if (bl_incomplete()) {
            UI_RunCRTCollapse();
            FwUpdate_RebootConsole();
        } else {
            confirm_clear(&s_confirm);
            App_RequestReboot();   // offers to save unsaved settings first
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------- render

static void render_board(int *row)
{
    const fw_board_info_t *b = FwUpdate_Board();
    const uint8_t *v = b->board.ver;
    switch (b->board.kind) {
    case FW_BOARD_XHD_APP:
        Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_NORMAL, "  Board : X-HD v%u.%u.%u, build %s (%s)",
                          v[1], v[2], v[3], b->board_build ? b->git : "unknown",
                          b->board_build ? b->built : "no about block");
        break;
    case FW_BOARD_XHD_BOOT:
        Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_AMBER,
                          "  Board : X-HD in UPDATE MODE (bootloader v%u.%u.%u)", v[1], v[2], v[3]);
        Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_AMBER,
                          "          An update did not finish. Choose Finish the update below.");
        break;
    case FW_BOARD_HDPLUS:
        Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_NORMAL,
                          "  Board : HD+ with its stock firmware (v%u.%u.%u)", v[0], v[1], v[2]);
        break;
    default:
        Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_AMBER, "  Board : no X-HD or HD+ answers on the SMBus");
        break;
    }
}

static void render_file(int *row, int item)
{
    const int n = FwUpdate_FileCount();
    const fw_file_t *f = FwUpdate_File(FwUpdate_Selected());
    if (!f) {
        Terminal_PrintItem((*row)++, item == s_sel, "Image : no .bin files in this XBE's folder");
        return;
    }
    s_file_row = *row;
    Terminal_PrintItem((*row)++, item == s_sel, "Image : %s  (%d of %d)",
                      f->name, FwUpdate_Selected() + 1, n);
    if (f->error) {
        Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_AMBER, "          Not usable: %s", f->error);
        return;
    }
    const fw_image_t *img = FwUpdate_Image();
    Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_NORMAL, "          build %s (%s), %u KB",
                      f->has_about ? f->git : "unknown", f->has_about ? f->built : "no about block",
                      (unsigned)(f->size / 1024));

    const fw_board_info_t *b = FwUpdate_Board();
    const bool xhd = b->board.kind == FW_BOARD_XHD_APP || b->board.kind == FW_BOARD_XHD_BOOT;
    if (xhd && img) {
        switch (verdict()) {
        case V_SAME:
            Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_DIM, "          Already installed: nothing to update.");
            break;
        case V_NEWER:
            Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_BRIGHT, "          UPDATE AVAILABLE: newer than the installed build.");
            break;
        case V_OLDER:
            Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_AMBER, "          OLDER than the installed build: this would downgrade.");
            break;
        case V_DIFFERENT:
            Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_NORMAL, b->board.kind == FW_BOARD_XHD_BOOT
                              ? "          Ready to finish the update."
                              : "          Differs from the board's firmware (age unknown).");
            break;
        case V_NONE:
            Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_DIM, (s_picked || n == 1)
                              ? "          not compared with the board yet"
                              : "          (A) on this line picks the image to compare");
            break;
        }
    }
    if (b->board.kind == FW_BOARD_XHD_APP && img) {
        if (b->have_cfg_magic && !fw_image_has_u32(img, b->cfg_magic)) {
            Terminal_PrintRow((*row)++, COLOR_PHOSPHOR_AMBER,
                              "          New settings layout: saved settings will reset to defaults.");
        }
    }
}

void Firmware_Render(void)
{
    int row = 6;
    Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "FIRMWARE UPDATE:");
    render_board(&row);

    for (int i = 0; i < s_item_count; i++) {
        switch (s_items[i]) {
        case IT_FILE:
            render_file(&row, i);
            row++;
            break;
        case IT_UPDATE:
            Terminal_PrintItem(row++, i == s_sel, "%s",
                              FwUpdate_Board()->board.kind == FW_BOARD_XHD_BOOT
                              ? "Finish the update with this image" : "Update firmware (changed pages only)");
            break;
        case IT_RETRY:
            Terminal_PrintItem(row++, i == s_sel, "Retry the update (bootloader not finished: keep the console on)");
            break;
        case IT_CONVERT:
            Terminal_PrintItem(row++, i == s_sel, "Convert this HD+ to X-HD");
            break;
        case IT_BACKUP:
            Terminal_PrintItem(row++, i == s_sel, "Back up the board's firmware to this XBE's folder");
            break;
        case IT_REBOOT:
            Terminal_PrintItem(row++, i == s_sel, "Reboot the console (power cycle)");
            break;
        }
    }

    Terminal_DrawHLine(row++, '-', COLOR_PHOSPHOR_DARK);
    fw_job_t job;
    uint32_t done, total;
    FwUpdate_GetProgress(&job, &done, &total);
    if (job != FW_JOB_NONE) {
        char bar[33];
        int fill = total ? (int)(done * 32 / total) : 0;
        for (int i = 0; i < 32; i++) bar[i] = i < fill ? '#' : '-';
        bar[32] = '\0';
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_BRIGHT, "  [%s] %u/%u  %s...", bar,
                          (unsigned)done, (unsigned)total, job_name(job));
    } else if (s_last_job != FW_JOB_NONE) {
        Terminal_PrintRow(row++, s_last_ok ? COLOR_PHOSPHOR_BRIGHT : COLOR_PHOSPHOR_AMBER, "  %s: %s",
                          job_name(s_last_job), s_last_ok ? "done" : "FAILED");
    } else {
        row++;
    }

    // Only the newest lines that fit above the footer.
    char lines[FW_LOG_LINES][FW_LOG_WIDTH];
    int n = FwUpdate_GetLog(lines);
    const int fit = Terminal_FooterTopRow() - row;
    for (int i = (fit > 0 && n > fit) ? n - fit : (fit > 0 ? 0 : n); i < n; i++) {
        Terminal_PrintRow(row++, i == n - 1 ? COLOR_PHOSPHOR_NORMAL : COLOR_PHOSPHOR_DIM, "  %s", lines[i]);
    }
}

const char *Firmware_Footer(void)
{
    if (FwUpdate_IsBusy()) {
        return "Working -- keep the console on";
    }
    if (bl_incomplete()) {
        return "(A): Retry the update -- keep the console on";
    }
    if (s_reboot_pending) {
        return "(A): Reboot the console to finish the update";
    }
    return "(A): Choose Image / Run Selected | (Y): Rescan Board & Files | (B): Cancel";
}
