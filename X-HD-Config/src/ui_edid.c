// SPDX-License-Identifier: GPL-3.0-only
#include "ui_edid.h"
#include "ui_terminal.h"
#include "smbus_hal.h"
#include <stdio.h>
#include <string.h>

static uint8_t       s_rawEdid[256];
static edid_parsed_t s_parsed;
static bool          s_hasData = false;
static bool          s_showHexDump = false;
static int           s_hexBlock = 0; // 0 = Block 0, 1 = Block 1

void EDID_Init(void)
{
    memset(s_rawEdid, 0, sizeof(s_rawEdid));
    memset(&s_parsed, 0, sizeof(s_parsed));
    s_hasData = false;
    s_showHexDump = false;
    s_hexBlock = 0;
}

void EDID_RefreshFromBoard(void)
{
    if (smbus_read_edid(s_rawEdid)) {
        Edid_Parse(s_rawEdid, &s_parsed);
        s_hasData = true;
    } else {
        s_hasData = false;
    }
}

bool EDID_HasData(void)
{
    return s_hasData;
}

const edid_parsed_t *EDID_GetParsed(void)
{
    return s_hasData ? &s_parsed : NULL;
}

void EDID_HandleInput(const input_state_t *in)
{
    if (!in) return;

    if (in->x) {
        s_showHexDump = !s_showHexDump;
    }
    if (in->y) {
        EDID_RefreshFromBoard();
    }
    if (in->a && s_showHexDump) {
        s_hexBlock ^= 1;
    }
    if (in->l_trigger) {
        s_hexBlock = 0;
    }
    if (in->r_trigger) {
        s_hexBlock = 1;
    }
}

void EDID_Render(void)
{
    int row = 6;

    if (!s_hasData) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER, "EDID STATUS: No display EDID cached or HDMI cable disconnected.");
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "Ensure HDMI cable is connected to active TV/monitor and press (Y) to read.");
        return;
    }

    if (!s_parsed.valid) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER, "EDID STATUS: The display's EDID is not valid (bad header).");
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL, "Press (Y) to re-read EDID buffer from X-HD board.");
        return;
    }

    // MODE 1: Decoded Summary
    if (!s_showHexDump) {
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "DISPLAY IDENTITY & HARDWARE METRICS:");

        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                          "  Sink Model Name   : %s (%s, P/N 0x%04X, Year %u)",
                          s_parsed.monitor_name, s_parsed.mfg_id, s_parsed.product_code, s_parsed.year);

        if (s_parsed.has_preferred_timing) {
            Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                              "  Native Display Res: %ux%u @ %.2f Hz (PCLK: %.2f MHz, Size: %ux%u cm)",
                              s_parsed.pref_width, s_parsed.pref_height, s_parsed.pref_refresh,
                              (float)s_parsed.pref_pclk_khz / 1000.0f, s_parsed.size_h_cm, s_parsed.size_v_cm);
        } else {
            Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                              "  Native Display Res: Unknown / Standard VESA timing (Size: %ux%u cm)",
                              s_parsed.size_h_cm, s_parsed.size_v_cm);
        }

        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                          "  Protocol & Sink   : %s (EDID v%u.%u, CEA Rev %u)",
                          s_parsed.is_hdmi ? "HDMI Sink [Active]" : "DVI-D Sink",
                          s_parsed.version_major, s_parsed.version_minor, s_parsed.cea_revision);

        Terminal_DrawHLine(row++, '-', COLOR_PHOSPHOR_DARK);
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "COLOUR FORMATS, CEC ADDRESS & AUDIO:");

        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                          "  Colour Formats    : RGB 4:4:4 [YES] | YCbCr 4:4:4 [%s] | YCbCr 4:2:2 [%s]",
                          s_parsed.ycbcr444 ? "YES" : "NO", s_parsed.ycbcr422 ? "YES" : "NO");

        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                          "  HDMI CEC Address  : %u.%u.%u.%u | Underscan / Overscan: [%s]",
                          s_parsed.cec_phys_addr[0], s_parsed.cec_phys_addr[1],
                          s_parsed.cec_phys_addr[2], s_parsed.cec_phys_addr[3],
                          s_parsed.underscan ? "YES" : "NO");

        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                          "  Audio Capabilities: Basic [%s] | Max %u-Ch | LPCM [%s] | Dolby [%s] | DTS [%s]",
                          s_parsed.basic_audio ? "YES" : "NO",
                          s_parsed.max_audio_channels,
                          s_parsed.lpcm_2ch ? "YES" : "NO",
                          s_parsed.dolby_digital ? "YES" : "NO",
                          s_parsed.dts ? "YES" : "NO");

        Terminal_DrawHLine(row++, '-', COLOR_PHOSPHOR_DARK);
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "SUPPORTED CEA RESOLUTIONS (From Video Data Block SVDs):");

        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                          "  Common Formats    : 720p60 [%s] | 1080i60 [%s] | 1080p60 [%s] | 480p [%s]",
                          s_parsed.supports_720p ? "YES" : "NO",
                          s_parsed.supports_1080i ? "YES" : "NO",
                          s_parsed.supports_1080p ? "YES" : "NO",
                          s_parsed.supports_480p ? "YES" : "NO");

        if (s_parsed.num_vics == 0) {
            Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                              "  Declared CEA VICs : None (DVI Sink / No Video Data Block)");
        } else {
            char vicLine1[128];
            char vicLine2[128];
            vicLine1[0] = '\0';
            vicLine2[0] = '\0';
            bool hasLine2 = false;

            for (int i = 0; i < s_parsed.num_vics; i++) {
                char tmp[16];
                if (!hasLine2) {
                    snprintf(tmp, sizeof(tmp), "%s%u", (vicLine1[0] != '\0') ? ", " : "", s_parsed.vics[i]);
                    // Only start line 2 if adding this VIC exceeds 66 characters
                    if (strlen(vicLine1) + strlen(tmp) <= 66) {
                        strncat(vicLine1, tmp, sizeof(vicLine1) - strlen(vicLine1) - 1);
                    } else {
                        hasLine2 = true;
                        snprintf(tmp, sizeof(tmp), "%u", s_parsed.vics[i]);
                        strncat(vicLine2, tmp, sizeof(vicLine2) - strlen(vicLine2) - 1);
                    }
                } else {
                    snprintf(tmp, sizeof(tmp), ", %u", s_parsed.vics[i]);
                    if (strlen(vicLine2) + strlen(tmp) <= 66) {
                        strncat(vicLine2, tmp, sizeof(vicLine2) - strlen(vicLine2) - 1);
                    } else {
                        strncat(vicLine2, ", ...", sizeof(vicLine2) - strlen(vicLine2) - 1);
                        break;
                    }
                }
            }

            Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                              "  Declared CEA VICs : %s", vicLine1);
            if (hasLine2 && vicLine2[0] != '\0') {
                Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                                  "                      %s", vicLine2);
            }
        }
    }
    // MODE 2: Raw Hex Dump
    else {
        int baseOffset = s_hexBlock * 128;
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM,
                          "RAW EDID HEX DUMP - BLOCK %u (%s, 128 BYTES):",
                          s_hexBlock, s_hexBlock == 0 ? "Base EDID Block 0" : "CEA-861 Extension Block 1");

        // 74 characters per row: offset field 9, hex 48, 1 gap, ASCII 16. At the 15px
        // mono cell (widest hex glyph 'A') that is 1110px, inside the 1120px margins.
        Terminal_PrintRowMono(row++, COLOR_PHOSPHOR_DIM,
                          "OFFSET   00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F ASCII DECODE");

        for (int line = 0; line < 8; line++) {
            int off = baseOffset + line * 16;
            char hexStr[64];
            char ascStr[20];

            snprintf(hexStr, sizeof(hexStr),
                     "%02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
                     s_rawEdid[off + 0], s_rawEdid[off + 1], s_rawEdid[off + 2], s_rawEdid[off + 3],
                     s_rawEdid[off + 4], s_rawEdid[off + 5], s_rawEdid[off + 6], s_rawEdid[off + 7],
                     s_rawEdid[off + 8], s_rawEdid[off + 9], s_rawEdid[off + 10], s_rawEdid[off + 11],
                     s_rawEdid[off + 12], s_rawEdid[off + 13], s_rawEdid[off + 14], s_rawEdid[off + 15]);

            for (int i = 0; i < 16; i++) {
                uint8_t c = s_rawEdid[off + i];
                ascStr[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
            }
            ascStr[16] = '\0';

            Terminal_PrintRowMono(row++, COLOR_PHOSPHOR_NORMAL,
                              "0x%04X:  %-48s %s", off, hexStr, ascStr);
        }

        Terminal_DrawHLine(row++, '-', COLOR_PHOSPHOR_DARK);
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_BRIGHT,
                          "  Viewing Block %u of 2 | (A) or [TRIGGERS] switch block",
                          s_hexBlock + 1);
    }
}
