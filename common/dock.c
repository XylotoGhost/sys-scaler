#include <stdio.h>
#include <string.h>
#include "dock.h"
#include "nvdisp.h"
#include "log.h"

// receiver caps, downstream ports, link config, branch device
static const u32 g_addr[4] = { 0x000, 0x080, 0x100, 0x500 };

void dockRead(u32 fd, DockInfo *d) {
    memset(d, 0, sizeof(*d));
    for (int i = 0; i < 4; i++) {
        NvdcDpaux a = { .cmd = 6, .addr = g_addr[i], .size = 16 };
        if (R_SUCCEEDED(nvIoctl(fd, NVDISP_GET_PANEL_DATA, &a))) memcpy(d->raw[i], a.data, 16);
    }
    const u8 *caps = d->raw[0], *dfp = d->raw[1], *link = d->raw[2], *branch = d->raw[3];

    d->dpcd_rev = caps[0];
    d->max_link_rate = caps[1];
    d->max_lanes = caps[2] & 0x1F;
    d->link_rate = link[0];
    d->lanes = link[1] & 0x1F;

    // DPCD 0x005 bit4: detailed capability info (4 bytes per downstream port at 0x080).
    if (caps[5] & 0x10) {
        d->dfp_type = dfp[0] & 0x07;
        if (d->dfp_type == 3 || d->dfp_type == 2) d->max_tmds_khz = dfp[1] * 2500;
    }

    memcpy(d->branch_oui, branch, 3);
    for (int i = 0; i < 6; i++) {
        u8 c = branch[3 + i];
        d->branch_id[i] = (c >= 0x20 && c < 0x7F) ? (char)c : 0;
    }
}

void dockLog(const DockInfo *d) {
    static const char *types[] = { "DP", "VGA", "DVI", "HDMI", "other", "?", "?", "?" };
    F("  DP link: rate 0x%02x x %u lanes (sink max 0x%02x x %u; 0x0A=HBR 0x14=HBR2)\n",
      d->link_rate, d->lanes, d->max_link_rate, d->max_lanes);
    F("  Dock converter: OUI %02x%02x%02x \"%s\", output %s, max TMDS %u MHz (0 = unknown)\n",
      d->branch_oui[0], d->branch_oui[1], d->branch_oui[2], d->branch_id, types[d->dfp_type & 7],
      d->max_tmds_khz / 1000);
    for (int i = 0; i < 4; i++) {
        char label[16];
        snprintf(label, sizeof(label), "DPCD 0x%03x", g_addr[i]);
        logHex(label, d->raw[i], 16);
    }
}
