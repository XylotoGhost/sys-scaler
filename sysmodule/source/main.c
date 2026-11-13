// sys-scaler: background sysmodule that applies the saved per-monitor profile
// while docked.
//
// Profile methods (see common/profile.h):
//  - centered: keep the stock 16:9 output (1080p or 720p) and set the vi display
//    magnification rect so the image is pre-squashed with bars; the monitor's
//    stretch to its panel makes it 16:9 again. Works for taller (16:10, 4:3) and
//    wider (21:9, 32:9) panels.
//  - sharp: switch the output to a native-aspect mode (1920x1200); vi draws the
//    image top-aligned, bar at the bottom.
//  - off: the monitor shows 16:9 correctly by itself; do nothing.
//
// Loop:
//  - every 250 ms: omm operation mode. Handheld -> reset zoom immediately (and
//    again after 1 s and 3 s, in case vi's own reconfiguration races us).
//  - every 1 s while docked: reload config.ini if its contents changed; read the
//    disp1 mode; (re)look up the profile by mode-DB key whenever the mode
//    settled after a change, and every 3 s while none is found (docking passes
//    through a temporary 480p state with a 13-mode DB).
//  - centered: sent once a 16:9 mode has been live for 1 s, then re-sent every
//    `refresh` seconds (default 1). Needed: the HOME menu startup after boot and
//    the power menu reset it without any event we can see, and vi has no getter.
//  - sharp: re-applied when the system resets it, max 5x in a row.
//
// Whenever no zoom should be active, the identity rect is sent once, also if
// this service didn't set the zoom itself: the app can leave a test zoom behind
// (closed via HOME), and so can an earlier instance of this service.
//
// Off switches: enabled=0 in config.ini, the pause file (holds the app's pid;
// ignored once that process is gone), or deleting the profile.
// Log: sdmc:/config/sys-scaler/log.txt (appended, restarted above 128 KB).

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

#include "log.h"
#include "nvdisp.h"
#include "aspect.h"
#include "profile.h"
#include "settings.h"
#include "vimag.h"

#define FAST_MS          250
#define SLOW_EVERY       4        // slow tick = every 4th fast tick (1 s)
#define SETTLE_MS        3000     // sharp: let the system finish its own mode set
#define ZOOM_SETTLE_MS   1000     // centered and profile lookup
#define LOOKUP_RETRY_MS  3000
#define STICK_MS         60000
#define MAX_APPLIES      5
#define LOG_MAX          (128 * 1024)

// ---------------------------------------------------------------------------
// libnx sysmodule boilerplate

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
// libnx's nv session: nvdrv:s is the only service a sysmodule can use for
// /dev/nvdisp-* (permission bit 8); the transfer memory comes out of the heap.
u32 __nx_nv_service_type = NvServiceType_System;
u32 __nx_nv_transfermem_size = 0x8000;

#define INNER_HEAP_SIZE 0x20000   // measured peak on device: 44 KB
void __libnx_initheap(void) {
    static char inner_heap[INNER_HEAP_SIZE];
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    fsdevMountSdmc();
    // sm stays open: omm / pm:info / vi:m / nvdrv:s are requested lazily.
}

void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    smExit();
}

// ---------------------------------------------------------------------------

static u32         g_fd;
static bool        g_nv_ok, g_fd_open;
static NvdcModeDB2 g_db;
static ViMag       g_vm;
static bool        g_omm_ok, g_pminfo_ok;
static Settings    g_set;
static u32         g_set_crc = 2;   // no value settingsFileCrc returns: forces the first load

#define T ((unsigned long long)nowMs())

static bool ensureNv(void) {
    if (g_fd_open) return true;
    if (!g_nv_ok) g_nv_ok = R_SUCCEEDED(nvInitialize());
    if (!g_nv_ok || R_FAILED(nvOpen(&g_fd, "/dev/nvdisp-disp1"))) return false;
    g_fd_open = true;
    return true;
}

// What vi last got from us. UNKNOWN: it may hold something else, because nothing
// was sent yet (boot, service restart) or the app ran a test in between.
typedef enum { ZOOM_UNKNOWN, ZOOM_IDENTITY, ZOOM_PROFILE } ZoomState;
static ZoomState g_zoom;

static bool zoomSend(const s32 ref[4], ZoomState st, const char *why) {
    static Result last_rc;
    s32 z[4] = {0};
    Result rc = vimagSetRef(&g_vm, ref, z);
    // Quiet for the periodic re-send and for a failure that just repeats.
    if (rc != last_rc || (R_SUCCEEDED(rc) && g_zoom != st))
        F("[%llu] zoom %s (%d,%d,%d,%d) on \"%s\": rc 0x%x\n", T, why, z[0], z[1], z[2], z[3],
          g_vm.display_name, rc);
    last_rc = rc;
    g_zoom = R_SUCCEEDED(rc) ? st : ZOOM_UNKNOWN;
    return R_SUCCEEDED(rc);
}

// Sends the identity rect unless that's what vi already has from us.
// force: send it regardless.
static void zoomReset(const char *why, bool force) {
    static const s32 identity[4] = { 0, 0, 1920, 1080 };
    if (force) g_zoom = ZOOM_UNKNOWN;
    if (g_zoom != ZOOM_IDENTITY) zoomSend(identity, ZOOM_IDENTITY, why);
}

// omm (IOperationModeManager) cmd 0 GetOperationMode -> u8 (0 handheld, 1 console).
// Raw IPC: libnx has no omm wrapper.
static Service g_omm;

// true = docked, or unknown (omm unavailable -> rely on disp1 alone)
static bool isDocked(void) {
    if (!g_omm_ok) g_omm_ok = R_SUCCEEDED(smGetService(&g_omm, "omm"));
    if (!g_omm_ok) return true;
    u8 m = 1;
    if (R_FAILED(serviceDispatchOut(&g_omm, 0, m))) return true;
    return m == 1;
}

// Paused by a running app test. A pause file whose app process is gone is
// deleted (an app closed via HOME never runs its cleanup).
static bool isPaused(void) {
    if (!fileExists(SS_PAUSE_FILE)) return false;
    u64 pid = 0, pgid = 0;
    if (!g_pminfo_ok) g_pminfo_ok = R_SUCCEEDED(pminfoInitialize());
    if (pauseReadPid(&pid) && g_pminfo_ok && R_SUCCEEDED(pminfoGetProgramId(&pgid, pid))) return true;
    F("[%llu] stale pause file (pid %llu gone), removing\n", T, (unsigned long long)pid);
    unlink(SS_PAUSE_FILE);
    return false;
}

static bool getMode(NvdcMode2 *m) {
    memset(m, 0, sizeof(*m));
    if (R_FAILED(nvIoctl(g_fd, NVDISP_GET_MODE2, m))) return false;
    return m->hActive && m->vActive && m->pclkKHz;
}

typedef struct {
    bool      connected;
    bool      have_profile;
    Profile   profile;
    u32       key;
    bool      need_lookup;
    u64       last_lookup;
    NvdcMode2 last;          // last observed live mode
    u64       last_change;   // when `last` started
    u64       good_since;    // sharp mode live since (0 = not live)
    int       applies;
    bool      gave_up;
    u64       zoom_sent;     // last zoom send (0 = not since the mode settled)
} Conn;

static void lookup(Conn *c, const NvdcMode2 *live, u64 t) {
    c->need_lookup = false;
    c->last_lookup = t;
    memset(&g_db, 0, sizeof(g_db));
    Result rc = nvIoctl(g_fd, NVDISP_GET_MODE_DB2, &g_db);
    if (R_FAILED(rc)) {
        F("[%llu] GET_MODE_DB2 failed (0x%x)\n", T, rc);
        return;
    }
    u32 key = profileKey(&g_db);
    Profile p;
    bool have = profileLoad(key, &p);
    if (key == c->key && have == c->have_profile) return;   // nothing new, stay quiet

    c->key = key;
    c->have_profile = have;
    if (have) {
        c->profile = p;
        c->zoom_sent = 0;
        c->applies = 0;
        c->gave_up = false;
    }
    F("[%llu] live %ux%u@%.2f, %u DB modes, key %08X, profile %s", T, live->hActive, live->vActive,
      nvdcHz(live), g_db.num_modes, key, have ? profileMethodName(p.method) : "none");
    if (have && p.name[0]) F(" \"%s\"", p.name);
    if (have && p.method == PROFILE_CENTERED) F(" (%d,%d,%d,%d)", p.zoom[0], p.zoom[1], p.zoom[2], p.zoom[3]);
    if (have && p.method == PROFILE_SHARP) F(" -> %ux%u", p.mode.hActive, p.mode.vActive);
    // Everything that allocates has run by now: the most this service took from its heap.
    F(", heap %d of %d KB\n", (int)(mallinfo().arena / 1024), INNER_HEAP_SIZE / 1024);
}

static void tickSharp(Conn *c, const NvdcMode2 *live, u64 t) {
    if (nvdcTimingEq(live, &c->profile.mode)) {
        if (!c->good_since) c->good_since = t;
        if (c->applies && t - c->good_since > STICK_MS) {
            c->applies = 0;
            c->gave_up = false;
        }
        return;
    }
    c->good_since = 0;
    if (live->hActive != 1920 || live->vActive != 1080) return;   // only from stock 1080p
    if (t - c->last_change < SETTLE_MS || c->gave_up) return;
    if (c->applies >= MAX_APPLIES) {
        F("[%llu] reset %d times in a row, giving up until reconnect\n", T, c->applies);
        c->gave_up = true;
        return;
    }
    NvdcMode2 io = c->profile.mode;
    Result rc = nvIoctl(g_fd, NVDISP_VALIDATE_MODE2, &io);
    if (R_FAILED(rc)) {
        F("[%llu] profile mode rejected by VALIDATE (0x%x), giving up\n", T, rc);
        c->gave_up = true;
        return;
    }
    io = c->profile.mode;
    rc = nvIoctl(g_fd, NVDISP_SET_MODE2, &io);
    c->applies++;
    F("[%llu] applied %ux%u (attempt %d): rc 0x%x\n", T, c->profile.mode.hActive,
      c->profile.mode.vActive, c->applies, rc);
}

static void tickCentered(Conn *c, const NvdcMode2 *live, u64 t) {
    if (!is16x9(live->hActive, live->vActive)) {
        zoomReset("output not 16:9", false);
        return;
    }
    if (t - c->last_change < ZOOM_SETTLE_MS) return;
    if (c->zoom_sent && (g_set.refresh_s == 0 || t - c->zoom_sent < (u64)g_set.refresh_s * 1000)) return;
    if (zoomSend(c->profile.zoom, ZOOM_PROFILE, "profile")) c->zoom_sent = t;
}

static void slowTick(Conn *c) {
    if (!ensureNv()) return;
    u64 t = nowMs();

    NvdcMode2 live;
    if (!getMode(&live)) {
        if (c->connected) F("[%llu] disp1 inactive\n", T);
        c->connected = false;
        zoomReset("disp1 inactive", false);
        return;
    }
    if (!c->connected) {
        memset(c, 0, sizeof(*c));
        c->connected = true;
        c->last = live;
        c->last_change = t;
        c->need_lookup = true;
        F("[%llu] disp1 active %ux%u@%.2f\n", T, live.hActive, live.vActive, nvdcHz(&live));
    }
    if (!nvdcTimingEq(&live, &c->last)) {
        F("[%llu] live mode changed to %ux%u@%.2f\n", T, live.hActive, live.vActive, nvdcHz(&live));
        c->last = live;
        c->last_change = t;
        c->zoom_sent = 0;
        // A mode we set ourselves doesn't change the monitor; anything else might.
        if (!(c->have_profile && c->profile.method == PROFILE_SHARP && nvdcTimingEq(&live, &c->profile.mode)))
            c->need_lookup = true;
    }

    bool settled = t - c->last_change >= ZOOM_SETTLE_MS;
    if ((c->need_lookup && settled) || (!c->have_profile && t - c->last_lookup >= LOOKUP_RETRY_MS))
        lookup(c, &live, t);

    if (!c->have_profile || c->profile.method == PROFILE_OFF) {
        zoomReset("no profile / off", false);
        return;
    }
    if (c->profile.method == PROFILE_CENTERED) tickCentered(c, &live, t);
    else tickSharp(c, &live, t);
}

// Returns true if config.ini changed (always on the first call).
static bool reloadSettings(void) {
    u32 crc = settingsFileCrc();
    if (crc == g_set_crc) return false;
    g_set_crc = crc;
    settingsLoad(&g_set);
    F("[%llu] settings: enabled %d, refresh %ds (rev %u)\n", T, g_set.enabled, g_set.refresh_s, g_set.rev);
    return true;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    ssMkdirs();
    logOpen(SS_LOG, LOG_MAX);
    u32 hv = hosversionGet();
    F("\n=== " SS_NAME " " SS_VERSION " started, HOS %u.%u.%u ===\n", HOSVER_MAJOR(hv), HOSVER_MINOR(hv),
      HOSVER_MICRO(hv));
    if (fileExists(SS_PAUSE_FILE)) {   // nothing can be running yet at boot
        unlink(SS_PAUSE_FILE);
        F("[%llu] removed leftover pause file\n", T);
    }

    Conn c = {0};
    bool was_docked = true, was_paused = false;
    u64 undock_at = 0;
    int resets_after_undock = 0;
    for (unsigned n = 0;; n++) {
        bool docked = isDocked();
        if (!docked) {
            if (was_docked) {
                F("[%llu] undocked (omm)\n", T);
                zoomReset("undocked", true);
                undock_at = nowMs();
                resets_after_undock = 0;
                c.connected = false;
            } else if (resets_after_undock < 2 &&
                       nowMs() - undock_at >= (resets_after_undock == 0 ? 1000ULL : 3000ULL)) {
                zoomReset("undocked, repeat", true);   // in case vi's switch-over raced us
                resets_after_undock++;
            }
        } else if (!was_docked) {
            F("[%llu] docked (omm)\n", T);
        }
        was_docked = docked;

        if (docked && n % SLOW_EVERY == 0) {
            if (reloadSettings()) c.connected = false;   // the app changed settings or a profile
            bool paused = isPaused();
            if (paused && !was_paused) {
                F("[%llu] paused by app\n", T);
                zoomReset("paused", false);   // hand the app a clean display, then stay out
            } else if (!paused && was_paused) {
                F("[%llu] pause ended\n", T);
                g_zoom = ZOOM_UNKNOWN;        // the app may have left its test zoom behind,
                c.connected = false;          // or saved or deleted a profile
            }
            was_paused = paused;

            if (!paused) {
                if (g_set.enabled) slowTick(&c);
                else zoomReset("disabled", false);
            }
        }
        svcSleepThread(FAST_MS * 1000000ULL);
    }
    return 0;
}
