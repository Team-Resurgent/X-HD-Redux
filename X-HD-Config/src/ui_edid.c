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

static void parse_edid(const uint8_t edid[256], edid_parsed_t *out)
{
    memset(out, 0, sizeof(edid_parsed_t));
    if (!edid) return;

    static const uint8_t kHeader[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (memcmp(edid, kHeader, 8) != 0) {
        out->valid = false;
        return;
    }
    out->valid = true;

    // Manufacturer ID (bytes 8-9: 3 letters, 5 bits each)
    uint16_t mfg = ((uint16_t)edid[8] << 8) | edid[9];
    out->mfg_id[0] = (char)(((mfg >> 10) & 0x1F) + 'A' - 1);
    out->mfg_id[1] = (char)(((mfg >> 5) & 0x1F) + 'A' - 1);
    out->mfg_id[2] = (char)((mfg & 0x1F) + 'A' - 1);
    out->mfg_id[3] = '\0';

    // Product Code, Serial, Year
    out->product_code = (uint16_t)edid[10] | ((uint16_t)edid[11] << 8);
    out->serial_num = (uint32_t)edid[12] | ((uint32_t)edid[13] << 8) |
                      ((uint32_t)edid[14] << 16) | ((uint32_t)edid[15] << 24);
    out->week = edid[16];
    out->year = (uint16_t)edid[17] + 1990;
    out->version_major = edid[18];
    out->version_minor = edid[19];
    out->size_h_cm = edid[21];
    out->size_v_cm = edid[22];

    // Search 18-byte descriptors (bytes 54, 72, 90, 108) for Monitor Name (0xFC)
    strncpy(out->monitor_name, "Generic Monitor", sizeof(out->monitor_name) - 1);
    out->monitor_name[sizeof(out->monitor_name) - 1] = '\0';
    for (int d = 0; d < 4; d++) {
        const uint8_t *desc = &edid[54 + d * 18];
        if (desc[0] == 0 && desc[1] == 0 && desc[2] == 0 && desc[3] == 0xFC) {
            int len = 0;
            for (int i = 5; i < 18; i++) {
                if (desc[i] == '\n' || desc[i] == '\r') break;
                if (desc[i] >= 0x20 && desc[i] <= 0x7E && len < (int)sizeof(out->monitor_name) - 1) {
                    out->monitor_name[len++] = (char)desc[i];
                }
            }
            out->monitor_name[len] = '\0';
            while (len > 0 && out->monitor_name[len - 1] == ' ') {
                out->monitor_name[--len] = '\0';
            }
            break;
        }
    }

    // Preferred Detailed Timing Descriptor 1 (bytes 54-71)
    uint32_t pclk = ((uint32_t)edid[54] | ((uint32_t)edid[55] << 8)) * 10; // kHz
    if (pclk > 0) {
        out->has_preferred_timing = true;
        out->pref_pclk_khz = pclk;
        out->pref_width = (uint16_t)edid[56] | (((uint16_t)edid[58] & 0xF0) << 4);
        uint16_t h_blank = (uint16_t)edid[57] | (((uint16_t)edid[58] & 0x0F) << 8);
        out->pref_height = (uint16_t)edid[59] | (((uint16_t)edid[61] & 0xF0) << 4);
        uint16_t v_blank = (uint16_t)edid[60] | (((uint16_t)edid[61] & 0x0F) << 8);
        uint32_t h_total = out->pref_width + h_blank;
        uint32_t v_total = out->pref_height + v_blank;
        if (h_total > 0 && v_total > 0) {
            out->pref_refresh = (double)(pclk * 1000) / (double)(h_total * v_total);
        }
    }

    // Block 1: CEA-861 Extension (byte 128)
    if (edid[128] == 0x02) {
        out->has_cea = true;
        out->cea_revision = edid[129];
        uint8_t flags = edid[131];
        out->underscan   = (flags & 0x80) != 0;
        out->basic_audio = (flags & 0x40) != 0;
        out->ycbcr444   = (flags & 0x20) != 0;
        out->ycbcr422   = (flags & 0x10) != 0;

        uint8_t d_offset = edid[130];
        if (d_offset > 4 && d_offset <= 127) {
            int pos = 132;
            while (pos < 128 + d_offset) {
                uint8_t tag = (edid[pos] >> 5) & 7;
                uint8_t len = edid[pos] & 0x1F;
                pos++;
                if (pos + len > 128 + d_offset || pos + len > 256) break;

                if (tag == 2) { // Video Data Block (VICs)
                    for (int i = 0; i < len && out->num_vics < 32; i++) {
                        uint8_t vic = edid[pos + i] & 0x7F;
                        out->vics[out->num_vics++] = vic;
                        if (vic == 2 || vic == 3)   out->supports_480p = true;
                        if (vic == 4 || vic == 19)  out->supports_720p = true;
                        if (vic == 5 || vic == 20)  out->supports_1080i = true;
                        if (vic == 16 || vic == 31) out->supports_1080p = true;
                    }
                } else if (tag == 1) { // Audio Data Block
                    for (int i = 0; i + 2 < len; i += 3) {
                        uint8_t format = (edid[pos + i] >> 3) & 0x0F;
                        uint8_t max_ch = (edid[pos + i] & 0x07) + 1;
                        if (max_ch > out->max_audio_channels) {
                            out->max_audio_channels = max_ch;
                        }
                        if (format == 1) {
                            out->lpcm_2ch = true;
                            if (max_ch > 2) out->lpcm_multich = true;
                        } else if (format == 2) {
                            out->dolby_digital = true;
                        } else if (format == 7) {
                            out->dts = true;
                        }
                    }
                } else if (tag == 3) { // Vendor Specific Data Block
                    if (len >= 3 && edid[pos] == 0x03 && edid[pos + 1] == 0x0C && edid[pos + 2] == 0x00) {
                        out->is_hdmi = true;
                        if (len >= 5) {
                            out->cec_phys_addr[0] = (edid[pos + 3] >> 4) & 0x0F;
                            out->cec_phys_addr[1] = edid[pos + 3] & 0x0F;
                            out->cec_phys_addr[2] = (edid[pos + 4] >> 4) & 0x0F;
                            out->cec_phys_addr[3] = edid[pos + 4] & 0x0F;
                        }
                    }
                }
                pos += len;
            }
        }

        // Sort declared VICs ascending (low to high) and deduplicate
        if (out->num_vics > 1) {
            for (int i = 0; i < out->num_vics - 1; i++) {
                for (int j = i + 1; j < out->num_vics; j++) {
                    if (out->vics[i] > out->vics[j]) {
                        uint8_t temp = out->vics[i];
                        out->vics[i] = out->vics[j];
                        out->vics[j] = temp;
                    }
                }
            }
            int unique = 0;
            for (int i = 0; i < out->num_vics; i++) {
                if (i == 0 || out->vics[i] != out->vics[i - 1]) {
                    out->vics[unique++] = out->vics[i];
                }
            }
            out->num_vics = unique;
        }
    }
}

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
        parse_edid(s_rawEdid, &s_parsed);
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
    if (in->left || in->l_trigger) {
        s_hexBlock = 0;
    }
    if (in->right || in->r_trigger) {
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
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_AMBER, "EDID STATUS: Invalid EDID Header magic received from sink.");
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
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM, "COLORSPACES, CEC ADDRESSING & AUDIO CAPABILITIES:");

        Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                          "  Colorspace Formats: RGB 4:4:4 [YES] | YCbCr 4:4:4 [%s] | YCbCr 4:2:2 [%s]",
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

        Terminal_PrintRow(row++, COLOR_PHOSPHOR_DIM,
                          "OFFSET    00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F   ASCII DECODE");

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

            Terminal_PrintRow(row++, COLOR_PHOSPHOR_NORMAL,
                              "0x%04X:   %-48s  %s", off, hexStr, ascStr);
        }

        Terminal_DrawHLine(row++, '-', COLOR_PHOSPHOR_DARK);
        Terminal_PrintRow(row++, COLOR_PHOSPHOR_BRIGHT,
                          "  Viewing Block %u of 2 | Press [D-PAD LEFT / RIGHT] or [TRIGGERS] to switch block",
                          s_hexBlock + 1);
    }
}
