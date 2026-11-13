// Display hardware access for the app: monitor info, output modes, vi zoom,
// and control of the background service.
#pragma once
#include <switch.h>
#include "aspect.h"
#include "dock.h"
#include "edid.h"
#include "nvdisp.h"
#include "nvsession.h"
#include "profile.h"

typedef struct {
    bool        docked;        // hwInit ran in docked mode
    bool        ready;         // display access works (nvdrv:s, disp1, mode DB)
    char        error[96];     // why not ready
    EdidInfo    edid;
    bool        have_edid;
    double      ar_auto;       // panel aspect from the EDID
    char        ar_auto_name[24];
    u32         key;           // profile key of the connected monitor
    NvdcModeDB2 db;
    DockInfo    dock;
    NvdcMode2   stock;         // 1920x1080@60 CEA restore mode
    bool        stock_ok;
} Hw;

extern Hw g_hw;

void hwInit(bool docked);
void hwExit(void);

bool   hwLiveMode(NvdcMode2 *m);
bool   hwIsStock(const NvdcMode2 *m);
Result hwSetMode(const NvdcMode2 *m);         // validates first
void   hwRestoreStock(void);                  // back to 1080p if something else is live
// Best "sharp" output mode for a panel aspect (>= 1920x1080, within the dock's
// TMDS limit, accepted by VALIDATE). Only for panels taller than 16:9.
bool   hwFindSharp(double ar, NvdcMode2 *out);

// vi magnification (own vi:m session, opened on first use).
Result hwZoomSet(const s32 ref[4]);   // reference units (1920x1080)
Result hwZoomReset(void);

// Monitor shape choices (Expert). Index 0 = Auto (EDID).
typedef struct { u32 w, h; const char *name; } Shape;
extern const Shape g_shapes[];
extern const int   g_num_shapes;
double shapeAr(int idx);              // idx 0 = auto
int    shapeFind(u32 w, u32 h);       // index for a stored aspect, 0 if none
void   arToFraction(double ar, u32 *w, u32 *h);

// Background service (sysmodule).
bool   sysmodRunning(void);
Result sysmodStart(void);
Result sysmodStop(void);
bool   sysmodInstalled(void);
bool   bootFlagGet(void);
bool   bootFlagSet(bool on);
