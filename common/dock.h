// DisplayPort-side info about the dock (DPCD over AUX via NVDISP_GET_PANEL_DATA).
#pragma once
#include <switch.h>
#include "nvsession.h"

typedef struct {
    bool ok;
    u8   dpcd_rev;            // 0x12 = DP 1.2
    u8   link_rate, lanes;    // active link (0x14 = HBR2)
    u8   max_link_rate, max_lanes;
    u8   dfp_type;            // downstream port type: 0 DP, 1 VGA, 2 DVI, 3 HDMI
    u32  max_tmds_khz;        // converter's max HDMI TMDS clock, 0 = unknown
    u8   branch_oui[3];
    char branch_id[7];
} DockInfo;

// Reads DPCD 0x000, 0x080, 0x100, 0x500 on an open /dev/nvdisp-disp1 fd.
// Raw bytes go to the log file; returns a decoded summary.
void dockRead(NvSession *s, u32 disp1_fd, DockInfo *out);
void dockLog(const DockInfo *d);   // one-screen summary
