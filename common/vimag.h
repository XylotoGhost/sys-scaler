// Standalone vi:m client for display magnification (ISystemDisplayService 1204,
// the system "Zoom" feature). Separate from libnx's global vi session because
// an app's window already holds the "Default" display there, and a session
// can't open the same display twice (vi error 0x1272).
#pragma once
#include <switch.h>

typedef struct {
    Service root, app, sys;
    u64     display_id;
    char    display_name[16];
    bool    open;
} ViMag;

// Tries the "Default" display, then "External".
Result vimagOpen(ViMag *v);
void   vimagClose(ViMag *v);

// Sets the magnification rect given in 1920x1080 reference units, scaled to the
// display's logical resolution. Opens the session on first use. sent (optional)
// receives the rect in logical units.
Result vimagSetRef(ViMag *v, const s32 ref[4], s32 sent[4]);
