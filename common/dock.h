// DisplayPort-side info about the dock (DPCD over AUX via NVDISP_GET_PANEL_DATA).
#pragma once
#include <switch.h>

typedef struct {
    u8   raw[4][16];          // DPCD 0x000, 0x080, 0x100, 0x500 (zero if a read failed)
    u8   dpcd_rev;            // 0x12 = DP 1.2
    u8   link_rate, lanes;    // active link (0x14 = HBR2)
    u8   max_link_rate, max_lanes;
    u8   dfp_type;            // downstream port type: 0 DP, 1 VGA, 2 DVI, 3 HDMI
    u32  max_tmds_khz;        // converter's max HDMI TMDS clock, 0 = unknown
    u8   branch_oui[3];
    char branch_id[7];
} DockInfo;

// Reads and decodes the DPCD on an open /dev/nvdisp-disp1 fd.
void dockRead(u32 disp1_fd, DockInfo *out);
// Summary and raw bytes to the log.
void dockLog(const DockInfo *d);
