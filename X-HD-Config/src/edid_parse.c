// SPDX-License-Identifier: GPL-3.0-only
#include "edid_parse.h"
#include <string.h>

void Edid_Parse(const uint8_t edid[256], edid_parsed_t *out)
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
