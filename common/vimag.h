// Standalone vi:m client for display magnification (ISystemDisplayService 1204,
// the system "Zoom" feature). Separate from libnx's global vi session because
// an app's console window already holds the "Default" display there, and a
// session can't open the same display twice (vi error 0x1272).
#pragma once
#include <switch.h>

typedef struct {
    Service root, app, sys;
    u64     display_id;
    char    display_name[16];
    bool    open;
} ViMag;

// Tries "Default", then "External". *tried_rc gets the first failure (may be NULL).
Result vimagOpen(ViMag *v, Result *first_rc);
void   vimagClose(ViMag *v);
Result vimagGetLogicalResolution(ViMag *v, s32 *w, s32 *h);
Result vimagSet(ViMag *v, s32 x, s32 y, s32 w, s32 h);
