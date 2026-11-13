// nvdisp ioctls and structs for /dev/nvdisp-disp1 (external display / dock).
// Sources: switchbrew.org "NV services" (checked 2026-11) and
// masagrator/SaltyNX saltysd_proc/source/display_refresh_rate.cpp, which uses
// GET_MODE2 / VALIDATE_MODE2 / SET_MODE2 / GET_MODE_DB2 from a sysmodule
// (nvdrv:s) to change the docked refresh rate.
#pragma once
#include <switch.h>
#include <assert.h>

#define NVDISP_GET_MODE2                0x803C021Bu
#define NVDISP_SET_MODE2                0x403C021Cu
#define NVDISP_VALIDATE_MODE2           0xC03C021Du
#define NVDISP_GET_MODE_DB2             0xEF20021Eu
#define NVDISP_GET_PANEL_DATA           0xC01C0226u // DPAUX access, used by SaltyNX with cmd 6 = read

// Mode2 struct (60 bytes), used by current firmware and SaltyNX.
// Field meanings decoded from GET_MODE_DB2 on HOS 23.0.1:
//   sync:  bit0 = HSync positive, bit1 = VSync positive (640x480 -> 0, DMT RB +H-V -> 1)
//   vmode: 0x200000 = EDID detailed timing, 0x400000 = CEA VIC, 0x80 = est./GTF, 0 = DMT;
//          VALIDATE_MODE2 returns 0x8000000 on success.
//   unk1:  CEA picture aspect flag, 0x40 = 4:3, 0x80 = 16:9 (Linux FB_FLAG_RATIO_*), 0 = none
//   bitsPerPixel: 24 in DB entries. GET_MODE2 returns garbage in bpp/unk1, so never
//          compare or re-apply those two fields from a GET.
#define NVDC_SYNC_HPOS     0x1
#define NVDC_SYNC_VPOS     0x2
#define NVDC_VMODE_DETAILED 0x200000
#define NVDC_VMODE_CEA      0x400000
#define NVDC_ASPECT_4_3     0x40
#define NVDC_ASPECT_16_9    0x80
typedef struct {
    u32 unk0;
    u32 hActive, vActive;
    u32 hSyncWidth, vSyncWidth;
    u32 hFrontPorch, vFrontPorch;
    u32 hBackPorch, vBackPorch;
    u32 pclkKHz;
    u32 bitsPerPixel;
    u32 vmode;
    u32 sync;
    u32 unk1;
    u32 reserved;
} NvdcMode2;
static_assert(sizeof(NvdcMode2) == 60, "NvdcMode2 size");

#define NVDC_DB_MAX 201
typedef struct {
    NvdcMode2 modes[NVDC_DB_MAX];
    u32 num_modes;
} NvdcModeDB2;
static_assert(sizeof(NvdcModeDB2) == 12064, "NvdcModeDB2 size");

// Number of valid entries (the driver's count, clamped to the array).
static inline u32 nvdcDbCount(const NvdcModeDB2 *db) {
    return db->num_modes > NVDC_DB_MAX ? NVDC_DB_MAX : db->num_modes;
}

// Same video timing (ignores bpp/vmode/unk1, which GET_MODE2 doesn't report reliably).
static inline bool nvdcTimingEq(const NvdcMode2 *a, const NvdcMode2 *b) {
    return a->hActive == b->hActive && a->vActive == b->vActive &&
           a->hSyncWidth == b->hSyncWidth && a->vSyncWidth == b->vSyncWidth &&
           a->hFrontPorch == b->hFrontPorch && a->vFrontPorch == b->vFrontPorch &&
           a->hBackPorch == b->hBackPorch && a->vBackPorch == b->vBackPorch &&
           a->pclkKHz == b->pclkKHz;
}

static inline double nvdcHz(const NvdcMode2 *m) {
    u32 ht = m->hActive + m->hFrontPorch + m->hSyncWidth + m->hBackPorch;
    u32 vt = m->vActive + m->vFrontPorch + m->vSyncWidth + m->vBackPorch;
    return (ht && vt) ? (double)m->pclkKHz * 1000.0 / ((double)ht * vt) : 0.0;
}

typedef struct {
    u32 cmd;   // 6 = DPCD read (as used by SaltyNX)
    u32 addr;  // DPCD address
    u32 size;  // <= 16
    u8  data[16];
} NvdcDpaux;
static_assert(sizeof(NvdcDpaux) == 0x1C, "NvdcDpaux size");
