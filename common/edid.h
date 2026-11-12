#pragma once
#include <switch.h>

typedef struct {
    u32  pclk_khz;
    u16  hact, hfp, hsw, hbp;
    u16  vact, vfp, vsw, vbp;
    bool hsync_pos, vsync_pos, interlaced;
} EdidTiming;

typedef struct { u16 w, h; } EdidRes;

#define EDID_MAX_DTDS 16
#define EDID_MAX_RES  48

typedef struct {
    bool       valid;              // header + base checksum OK
    int        num_blocks;         // 1 + extension count (clamped to buffer)
    char       name[14];
    EdidTiming dtds[EDID_MAX_DTDS];
    int        num_dtds;
    u32        native_w, native_h; // preferred (first base-block) DTD, 0 if none
    int        max_pclk_mhz;       // from range limits descriptor, 0 if absent
    int        screen_w_cm, screen_h_cm;
    EdidRes    res[EDID_MAX_RES];  // every distinct resolution from DTDs / standard timings
    int        num_res;
} EdidInfo;

// Parses and logs (file + short screen summary).
void edidParse(const u8 *buf, size_t len, EdidInfo *out);
