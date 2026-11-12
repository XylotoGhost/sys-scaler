#include <stdio.h>
#include <string.h>
#include "dock.h"
#include "nvdisp.h"
#include "log.h"

static bool dpcdRead(NvSession *s, u32 fd, u32 addr, const char *what, u8 out[16]) {
    NvdcDpaux a = { .cmd = 6, .addr = addr, .size = 16 };
    Result rc = nvsIoctl(s, fd, NVDISP_GET_PANEL_DATA, &a);
    if (R_FAILED(rc)) {
        F("  DPCD 0x%03x (%s): failed 0x%x nv 0x%x\n", addr, what, rc, s->last_nv_error);
        memset(out, 0, 16);
        return false;
    }
    char label[48];
    snprintf(label, sizeof(label), "DPCD 0x%03x (%s)", addr, what);
    logHex(label, a.data, 16);
    memcpy(out, a.data, 16);
    return true;
}

void dockRead(NvSession *s, u32 fd, DockInfo *d) {
    memset(d, 0, sizeof(*d));
    u8 caps[16], dfp[16], link[16], branch[16];
    d->ok = dpcdRead(s, fd, 0x000, "receiver caps", caps);
    dpcdRead(s, fd, 0x080, "downstream ports", dfp);
    dpcdRead(s, fd, 0x100, "link config", link);
    dpcdRead(s, fd, 0x500, "branch device", branch);

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
    L("  DP link: rate 0x%02x x %u lanes (sink max 0x%02x x %u; 0x0A=HBR 0x14=HBR2)\n",
      d->link_rate, d->lanes, d->max_link_rate, d->max_lanes);
    L("  Dock converter: OUI %02x%02x%02x \"%s\", output %s, max TMDS %s",
      d->branch_oui[0], d->branch_oui[1], d->branch_oui[2], d->branch_id, types[d->dfp_type & 7],
      d->max_tmds_khz ? "" : "unknown");
    if (d->max_tmds_khz) L("%u MHz", d->max_tmds_khz / 1000);
    L("\n");
}
