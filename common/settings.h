// Paths and the global settings file shared by the app and the sysmodule.
//
// sdmc:/config/sys-scaler/
//   config.ini        global settings (this file); defaults apply while it's missing
//   profiles/*.ini    one per monitor (see profile.h)
//   pause             exists while the app runs a test; holds the app's pid
//   log.txt           sysmodule log
//   app-log.txt       app log
//   diag/             diagnostics export
//
// The sysmodule re-reads config.ini whenever its contents change (checked every
// second while docked). The app bumps `rev` on every change, including profile
// changes, so the sysmodule also re-reads the profile.
#pragma once
#include <switch.h>

#define SS_NAME        "sys-scaler"
#define SS_TID         0x420000000000A5F1ULL
#define SS_DIR         "sdmc:/config/sys-scaler"
#define SS_CONFIG      SS_DIR "/config.ini"
#define SS_PROFILE_DIR SS_DIR "/profiles"
#define SS_PAUSE_FILE  SS_DIR "/pause"
#define SS_LOG         SS_DIR "/log.txt"
#define SS_APP_LOG     SS_DIR "/app-log.txt"
#define SS_DIAG_DIR    SS_DIR "/diag"
#define SS_CONTENT_DIR "sdmc:/atmosphere/contents/420000000000A5F1"
#define SS_BOOT_FLAG   SS_CONTENT_DIR "/flags/boot2.flag"

#define REFRESH_DEFAULT 1

typedef struct {
    bool enabled;     // master switch
    int  refresh_s;   // zoom re-send interval in seconds, 0 = only after events
    bool expert;      // app shows Expert mode
    u32  rev;         // bumped by the app on every change
} Settings;

void settingsDefaults(Settings *s);
bool settingsLoad(Settings *s);     // false if missing (s then holds defaults)
bool settingsSave(Settings *s);     // bumps s->rev
u32  settingsFileCrc(void);         // CRC of config.ini's contents (always odd), 0 if missing

void ssMkdirs(void);

bool fileExists(const char *path);
bool touchFile(const char *path);

// Pause file: holds the pid of the app process that created it, so a pause left
// behind by an app closed via HOME can be detected as stale.
bool pauseWrite(void);
bool pauseReadPid(u64 *pid);
void pauseRemoveIfOurs(void);
