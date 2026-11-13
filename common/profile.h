// Per-monitor profiles: what to do when a given monitor is connected.
//
// Key = the timings in the driver's mode DB for the connected display (derived
// from the monitor EDID as seen through the dock), so a different monitor or TV
// never matches. Only the timings count: the key doesn't depend on the order of
// the DB or on its flag and unknown fields. A profile is only written after the
// user confirmed the result in the app.
//
// File: sdmc:/config/sys-scaler/profiles/<KEY>.ini
//   name=PE160WUT           monitor name (display only)
//   method=centered | sharp | off
//   aspect=16:10            panel shape it was set up for
//   zoom=x,y,w,h            centered: vi magnification rect in 1920x1080 units
//                           (scaled to the actual logical resolution at apply time)
//   mode2=<15 u32 fields>   sharp: output mode, e.g. 1920x1200
#pragma once
#include <switch.h>
#include "nvdisp.h"

typedef enum {
    PROFILE_SHARP    = 0,   // native-aspect output mode, image top-aligned
    PROFILE_CENTERED = 1,   // stock 16:9 output + vi magnification (pre-squash)
    PROFILE_OFF      = 2,   // monitor already shows 16:9 correctly: do nothing
} ProfileMethod;

typedef struct {
    ProfileMethod method;
    u32       ar_w, ar_h;   // panel shape, 0 = unknown
    NvdcMode2 mode;         // SHARP
    s32       zoom[4];      // CENTERED: x, y, w, h in 1920x1080 reference units
    char      name[16];
} Profile;

#define PROFILE_MAX 64

u32  profileKey(const NvdcModeDB2 *db);
bool profileLoad(u32 key, Profile *out);
bool profileSave(u32 key, const Profile *p);
bool profileDelete(u32 key);
int  profileDeleteAll(void);            // returns the number of files removed
int  profileList(u32 *keys, int max);   // keys of all saved profiles
const char *profileMethodName(ProfileMethod m);   // "Centered", "Sharp", "Off"
