#include <string.h>
#include <stdio.h>
#include "edid.h"
#include "log.h"

static void noteRes(EdidInfo *info, u32 w, u32 h) {
    for (int i = 0; i < info->num_res; i++)
        if (info->res[i].w == w && info->res[i].h == h) return;
    if (info->num_res < EDID_MAX_RES) info->res[info->num_res++] = (EdidRes){ (u16)w, (u16)h };
}

static double timingHz(const EdidTiming *t) {
    u32 ht = t->hact + t->hfp + t->hsw + t->hbp;
    u32 vt = t->vact + t->vfp + t->vsw + t->vbp;
    return (ht && vt) ? (double)t->pclk_khz * 1000.0 / ((double)ht * vt) : 0.0;
}

static void parseDtd(const u8 *d, EdidInfo *info, const char *where) {
    EdidTiming t = {0};
    t.pclk_khz  = (u32)(d[0] | (d[1] << 8)) * 10;
    t.hact      = d[2] | ((d[4] & 0xF0) << 4);
    u16 hblank  = d[3] | ((d[4] & 0x0F) << 8);
    t.vact      = d[5] | ((d[7] & 0xF0) << 4);
    u16 vblank  = d[6] | ((d[7] & 0x0F) << 8);
    t.hfp       = d[8] | ((d[11] & 0xC0) << 2);
    t.hsw       = d[9] | ((d[11] & 0x30) << 4);
    t.vfp       = (d[10] >> 4) | ((d[11] & 0x0C) << 2);
    t.vsw       = (d[10] & 0x0F) | ((d[11] & 0x03) << 4);
    t.hbp       = hblank - t.hfp - t.hsw;
    t.vbp       = vblank - t.vfp - t.vsw;
    t.interlaced = d[17] & 0x80;
    bool digital_separate = (d[17] & 0x18) == 0x18;
    t.hsync_pos = digital_separate && (d[17] & 0x02);
    t.vsync_pos = digital_separate && (d[17] & 0x04);

    F("  DTD [%s] %ux%u%s @ %.3f Hz  pclk %u kHz\n", where, t.hact, t.vact,
      t.interlaced ? "i" : "p", timingHz(&t), t.pclk_khz);
    F("      H: fp %u sync %u bp %u (total %u)  V: fp %u sync %u bp %u (total %u)  sync %cH %cV%s\n",
      t.hfp, t.hsw, t.hbp, t.hact + hblank, t.vfp, t.vsw, t.vbp, t.vact + vblank,
      t.hsync_pos ? '+' : '-', t.vsync_pos ? '+' : '-',
      digital_separate ? "" : " (not digital-separate sync)");

    noteRes(info, t.hact, t.vact);
    if (info->num_dtds < EDID_MAX_DTDS) info->dtds[info->num_dtds++] = t;
}

static void parseStdTiming(u8 b0, u8 b1, EdidInfo *info, u8 edid_rev) {
    if ((b0 == 0x01 && b1 == 0x01) || b0 == 0x00) return;
    u32 h = (b0 + 31) * 8, v = 0;
    const char *ar = "?";
    switch (b1 >> 6) {
        case 0: if (edid_rev >= 3) { v = h * 10 / 16; ar = "16:10"; } else { v = h; ar = "1:1"; } break;
        case 1: v = h * 3 / 4;  ar = "4:3";  break;
        case 2: v = h * 4 / 5;  ar = "5:4";  break;
        case 3: v = h * 9 / 16; ar = "16:9"; break;
    }
    F("  STD %ux%u @ %u Hz (%s)\n", h, v, (b1 & 0x3F) + 60, ar);
    noteRes(info, h, v);
}

static void parseDescriptor(const u8 *d, EdidInfo *info, u8 edid_rev) {
    u8 tag = d[3];
    char text[14] = {0};
    switch (tag) {
        case 0xFC: case 0xFF: case 0xFE:
            for (int i = 0; i < 13 && d[5 + i] != 0x0A; i++) text[i] = (char)d[5 + i];
            F("  %s: \"%s\"\n", tag == 0xFC ? "Name" : tag == 0xFF ? "Serial" : "Text", text);
            if (tag == 0xFC) memcpy(info->name, text, sizeof(text));
            break;
        case 0xFD:
            info->max_pclk_mhz = d[9] * 10;
            F("  Range limits: V %u-%u Hz, H %u-%u kHz, max pclk %u MHz\n",
              d[5], d[6], d[7], d[8], d[9] * 10);
            break;
        case 0xFA:
            for (int i = 0; i < 6; i++) parseStdTiming(d[5 + i * 2], d[6 + i * 2], info, edid_rev);
            break;
        default:
            F("  Descriptor tag 0x%02x (not decoded)\n", tag);
            break;
    }
}

static const char *vicName(u8 vic) {
    switch (vic) {
        case 1:  return "640x480p60";
        case 2: case 3: return "720x480p60";
        case 4:  return "1280x720p60";
        case 5:  return "1920x1080i60";
        case 16: return "1920x1080p60";
        case 17: case 18: return "720x576p50";
        case 19: return "1280x720p50";
        case 31: return "1920x1080p50";
        case 32: return "1920x1080p24";
        case 33: return "1920x1080p25";
        case 34: return "1920x1080p30";
        case 63: return "1920x1080p120";
        case 64: return "1920x1080p100";
        case 93: case 94: case 95: return "3840x2160p24-30";
        case 96: case 97: return "3840x2160p50/60";
        default: return "";
    }
}

static void parseCea(const u8 *b, EdidInfo *info) {
    u8 dtd_off = b[2];
    F("  CEA-861 rev %u, flags 0x%02x (underscan %s, basic audio %s, YCbCr444 %s, YCbCr422 %s)\n",
      b[1], b[3], (b[3] & 0x80) ? "yes" : "no", (b[3] & 0x40) ? "yes" : "no",
      (b[3] & 0x20) ? "yes" : "no", (b[3] & 0x10) ? "yes" : "no");

    if (dtd_off >= 4 && dtd_off <= 127) {
        for (u8 i = 4; i < dtd_off;) {
            u8 tag = b[i] >> 5, blen = b[i] & 0x1F;
            const u8 *p = &b[i + 1];
            if (i + 1 + blen > dtd_off) break;
            if (tag == 2) {
                F("  Video data block (SVDs):");
                for (u8 j = 0; j < blen; j++) {
                    u8 svd = p[j];
                    bool native = svd >= 129 && svd <= 192;
                    u8 vic = native ? (svd & 0x7F) : svd;
                    F(" %u%s%s%s%s", vic, native ? "*" : "",
                      vicName(vic)[0] ? "(" : "", vicName(vic), vicName(vic)[0] ? ")" : "");
                }
                F("\n");
            } else if (tag == 3 && blen >= 3) {
                u32 oui = p[0] | (p[1] << 8) | (p[2] << 16);
                if (oui == 0x000C03) {
                    F("  HDMI 1.x VSDB, phys addr %u.%u.%u.%u", p[3] >> 4, p[3] & 0xF, p[4] >> 4, p[4] & 0xF);
                    if (blen >= 7) F(", max TMDS %u MHz", p[6] * 5);
                    F("\n");
                } else if (oui == 0xC45DD8) {
                    F("  HDMI Forum VSDB, max TMDS %u MHz\n", blen >= 5 ? p[4] * 5 : 0);
                } else {
                    F("  Vendor block OUI %06x, len %u\n", oui, blen);
                }
            } else if (tag == 7 && blen >= 1) {
                F("  Extended tag %u, len %u", p[0], blen);
                if (p[0] == 0 && blen >= 2)
                    F(" (Video Capability: QY %u QS %u, PT %u IT %u CE %u overscan behaviour)",
                      (p[1] >> 7) & 1, (p[1] >> 6) & 1, (p[1] >> 4) & 3, (p[1] >> 2) & 3, p[1] & 3);
                F("\n");
            } else {
                F("  Data block tag %u, len %u\n", tag, blen);
            }
            i += 1 + blen;
        }
        for (u8 i = dtd_off; i + 18 <= 127; i += 18) {
            if (b[i] == 0 && b[i + 1] == 0) break;
            parseDtd(&b[i], info, "CEA");
        }
    }
}

void edidParse(const u8 *buf, size_t len, EdidInfo *info) {
    static const u8 hdr[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    memset(info, 0, sizeof(*info));
    if (len < 128 || memcmp(buf, hdr, 8) != 0) {
        L("  EDID: no valid header.\n");
        return;
    }
    u8 sum = 0;
    for (int i = 0; i < 128; i++) sum += buf[i];
    info->valid = (sum == 0);

    u16 mfg = (buf[8] << 8) | buf[9];
    char pnp[4] = { (char)('A' - 1 + ((mfg >> 10) & 0x1F)), (char)('A' - 1 + ((mfg >> 5) & 0x1F)),
                    (char)('A' - 1 + (mfg & 0x1F)), 0 };
    u8 rev = buf[19];
    info->screen_w_cm = buf[21];
    info->screen_h_cm = buf[22];
    int ext = buf[126];
    info->num_blocks = 1 + ext;
    if ((size_t)info->num_blocks * 128 > len) info->num_blocks = (int)(len / 128);

    F("EDID %u.%u, mfg %s, product 0x%04x, %u ext block(s), base checksum %s\n",
      buf[18], rev, pnp, buf[10] | (buf[11] << 8), ext, info->valid ? "OK" : "BAD");
    F("  Screen size: %u x %u cm\n", info->screen_w_cm, info->screen_h_cm);
    F("  Established timings: %02x %02x %02x\n", buf[35], buf[36], buf[37]);

    for (int i = 0; i < 8; i++) parseStdTiming(buf[38 + i * 2], buf[39 + i * 2], info, rev);
    for (int i = 0; i < 4; i++) {
        const u8 *d = &buf[54 + i * 18];
        if (d[0] || d[1]) {
            parseDtd(d, info, i == 0 ? "base, preferred" : "base");
            if (i == 0 && info->num_dtds > 0) {
                info->native_w = info->dtds[0].hact;
                info->native_h = info->dtds[0].vact;
            }
        } else {
            parseDescriptor(d, info, rev);
        }
    }

    for (int blk = 1; blk < info->num_blocks; blk++) {
        const u8 *b = &buf[blk * 128];
        F("Extension block %d: tag 0x%02x\n", blk, b[0]);
        if (b[0] == 0x02) parseCea(b, info);
        else if (b[0] == 0x70) F("  DisplayID extension (not decoded, see raw dump)\n");
        else if (b[0] == 0xF0) F("  Block map\n");
    }

    L("  Monitor: \"%s\" (%s), %ux%u cm, %d block(s), checksum %s\n",
      info->name, pnp, info->screen_w_cm, info->screen_h_cm, info->num_blocks,
      info->valid ? "OK" : "BAD");
    if (info->native_w) {
        const EdidTiming *p = &info->dtds[0];
        L("  Preferred: %ux%u @ %.2f Hz, pclk %u kHz\n", p->hact, p->vact, timingHz(p), p->pclk_khz);
    }
    L("  Resolutions advertised:");
    for (int i = 0; i < info->num_res; i++) L(" %ux%u", info->res[i].w, info->res[i].h);
    L("\n  Max pclk (range limits): %d MHz\n", info->max_pclk_mhz);
}
