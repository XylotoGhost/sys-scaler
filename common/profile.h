// Per-monitor profiles: which output mode to apply automatically.
//
// Key = CRC32 of the driver's mode DB for the connected display (derived from
// the monitor EDID as seen through the dock), so a different monitor or TV
// never matches. A profile is only written after the user confirmed the mode
// is visible in modetest.
//
// File: sdmc:/aspectfix/profiles/<KEY>.txt
//   # comment
//   method=zoom | mode
//   base=1920x1080          (only apply while the system runs this mode)
//   zoom=x,y,w,h            (method=zoom: vi display magnification rect, "centered")
//   mode2=<15 u32 fields>   (method=mode: output mode, e.g. 1920x1200, "sharp")
#pragma once
#include <switch.h>
#include "nvdisp.h"

#define AF_DIR          "sdmc:/aspectfix"
#define AF_PROFILE_DIR  AF_DIR "/profiles"
#define AF_PAUSE_FILE   AF_DIR "/pause"     // sysmodule idles while this exists
#define AF_DISABLE_FILE AF_DIR "/disabled"  // permanent off switch

typedef enum {
    PROFILE_MODE = 0,   // sharp: native-aspect output mode, image top-aligned
    PROFILE_ZOOM = 1,   // centered: stock mode + vi magnification (pre-squash)
} ProfileMethod;

typedef struct {
    ProfileMethod method;
    u32       base_w, base_h;
    NvdcMode2 mode;      // PROFILE_MODE
    s32       zoom[4];   // PROFILE_ZOOM: x, y, w, h
} Profile;

u32  profileKey(const NvdcModeDB2 *db);
bool profileLoad(u32 key, Profile *out);
bool profileSave(u32 key, const Profile *p, const char *monitor_name);
bool profileDelete(u32 key);
int  profileDeleteAll(void);   // returns number of files removed

bool fileExists(const char *path);
bool touchFile(const char *path);

// Pause file holds the pid of the modetest process that created it, so a pause
// left behind by a modetest that was closed via HOME (no clean exit, run 6) can
// be detected as stale.
bool pauseWrite(void);              // modetest: write own pid
bool pauseReadPid(u64 *pid);        // false if missing or unparsable
