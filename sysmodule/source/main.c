// aspectfixd: background sysmodule that applies the saved per-monitor profile
// while docked.
//
// Two profile methods (see common/profile.h):
//  - zoom ("centered"): keep the stock 1920x1080 output and set the vi display
//    magnification rect so the image is pre-squashed with bars top and bottom;
//    the monitor's stretch to its panel makes it 16:9 again.
//  - mode ("sharp"): switch the output to a native-aspect mode (1920x1200);
//    vi draws the image top-aligned, bar at the bottom.
//
// Loop:
//  - every 250 ms: omm operation mode. Handheld -> reset zoom immediately (and
//    again after 1 s and 3 s, in case vi's own reconfiguration races us).
//  - every 1 s while docked: read disp1 mode; (re)look up the profile by mode-DB
//    CRC whenever the mode settled after a change, and every 3 s while none is
//    found (docking passes through a temporary 480p state with a 13-mode DB).
//  - zoom: sent once the base mode has been live for 1 s, then re-sent every
//    N s (sdmc:/aspectfix/refresh.txt, default 1, 0 = never). Needed: the HOME
//    menu startup after boot and the power menu reset it without any event we
//    can see (run 8), and vi has no getter. mode: re-applied when reset, max 5x.
//
// Off switches: sdmc:/aspectfix/disabled, sdmc:/aspectfix/pause (holds modetest's
// pid; ignored once that process is gone), or delete the profile file.
// Log: sdmc:/aspectfix/aspectfixd.txt (appended, restarted above 128 KB).

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "log.h"
#include "nvsession.h"
#include "nvdisp.h"
#include "profile.h"
#include "vimag.h"

#define VERSION          "0.4.0"
#define NV_TMEM          0x8000
#define FAST_MS          250
#define SLOW_EVERY       4        // slow tick = every 4th fast tick (1 s)
#define SETTLE_MS        3000     // mode profiles: let the system finish its own mode set
#define ZOOM_SETTLE_MS   1000     // zoom profiles and profile lookup
#define REFRESH_DEFAULT  1
#define REFRESH_FILE     AF_DIR "/refresh.txt"
#define LOOKUP_RETRY_MS  3000
#define STICK_MS         60000
#define MAX_APPLIES      5
#define LOG_MAX          (128 * 1024)

// ---------------------------------------------------------------------------
// libnx sysmodule boilerplate

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x60000
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

static NvSession   g_nv;
static u32         g_fd;
static bool        g_fd_open;
static NvdcModeDB2 g_db;
static ViMag       g_vm;
static bool        g_zoom_active;   // we set a non-identity magnification
static bool        g_omm_ok, g_pminfo_ok;

static u64 nowMs(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}
#define T ((unsigned long long)nowMs())

static bool ensureNv(void) {
    if (g_fd_open) return true;
    if (!g_nv.active && R_FAILED(nvsOpen(&g_nv, "nvdrv:s", NV_TMEM, NULL))) return false;
    if (R_FAILED(nvsDevOpen(&g_nv, "/dev/nvdisp-disp1", &g_fd))) return false;
    g_fd_open = true;
    return true;
}

static bool ensureVi(void) {
    static bool logged;
    if (g_vm.open) return true;
    Result rc = vimagOpen(&g_vm, NULL);
    if (R_FAILED(rc)) {
        if (!logged) F("[%llu] vi:m open failed 0x%x (will retry)\n", T, rc);
        logged = true;
        return false;
    }
    F("[%llu] vi:m open, display \"%s\"\n", T, g_vm.display_name);
    return true;
}

static bool zoomApply(const s32 z[4]) {
    if (!ensureVi()) return false;
    Result rc = vimagSet(&g_vm, z[0], z[1], z[2], z[3]);
    if (!g_zoom_active || R_FAILED(rc))
        F("[%llu] zoom (%d,%d,%d,%d): rc 0x%x\n", T, z[0], z[1], z[2], z[3], rc);
    g_zoom_active = R_SUCCEEDED(rc);
    return g_zoom_active;
}

// force: send identity even if we don't think a zoom is active.
static void zoomReset(const char *why, bool force) {
    if (!g_zoom_active && !force) return;
    bool was = g_zoom_active;
    g_zoom_active = false;
    if (!ensureVi()) return;
    s32 lw = 1920, lh = 1080;
    vimagGetLogicalResolution(&g_vm, &lw, &lh);
    Result rc = vimagSet(&g_vm, 0, 0, lw, lh);
    if (was || R_FAILED(rc))
        F("[%llu] zoom reset (%s) to (0,0,%d,%d): rc 0x%x\n", T, why, lw, lh, rc);
}

// omm (IOperationModeManager) cmd 0 GetOperationMode -> u8 (0 handheld, 1 console).
// Raw IPC: the libnx in the devkitpro image predates its omm wrapper.
static Service g_omm;

// true = docked, or unknown (omm unavailable -> rely on disp1 alone)
static bool isDocked(void) {
    if (!g_omm_ok) g_omm_ok = R_SUCCEEDED(smGetService(&g_omm, "omm"));
    if (!g_omm_ok) return true;
    u8 m = 1;
    if (R_FAILED(serviceDispatchOut(&g_omm, 0, m))) return true;
    return m == 1;
}

// Pause/disable switches. A pause file whose modetest process is gone is
// deleted (modetest closed via HOME never runs its cleanup).
static bool isIdle(void) {
    if (fileExists(AF_DISABLE_FILE)) return true;
    if (!fileExists(AF_PAUSE_FILE)) return false;
    u64 pid = 0, pgid = 0;
    if (!g_pminfo_ok) g_pminfo_ok = R_SUCCEEDED(pminfoInitialize());
    bool alive = pauseReadPid(&pid) && g_pminfo_ok && R_SUCCEEDED(pminfoGetProgramId(&pgid, pid));
    if (alive) return true;
    F("[%llu] stale pause file (pid %llu gone), removing\n", T, (unsigned long long)pid);
    unlink(AF_PAUSE_FILE);
    return false;
}

static bool getMode(NvdcMode2 *m) {
    memset(m, 0, sizeof(*m));
    if (R_FAILED(nvsIoctl(&g_nv, g_fd, NVDISP_GET_MODE2, m))) return false;
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
    u64       good_since;    // profile mode live since (0 = not live)
    int       applies;
    bool      gave_up;
    u64       zoom_sent;     // last zoom send (0 = not since the mode settled)
    int       refresh_s;     // zoom re-send interval, 0 = only after events
} Conn;

// sdmc:/aspectfix/refresh.txt: zoom re-send interval in seconds (0 = off).
// Read on every (re)connect so it can be changed without a reboot.
static int readRefresh(void) {
    FILE *f = fopen(REFRESH_FILE, "r");
    if (!f) return REFRESH_DEFAULT;
    int v = REFRESH_DEFAULT;
    if (fscanf(f, "%d", &v) != 1 || v < 0 || v > 3600) v = REFRESH_DEFAULT;
    fclose(f);
    return v;
}

static void lookup(Conn *c, const NvdcMode2 *live, u64 t) {
    c->need_lookup = false;
    c->last_lookup = t;
    memset(&g_db, 0, sizeof(g_db));
    if (R_FAILED(nvsIoctl(&g_nv, g_fd, NVDISP_GET_MODE_DB2, &g_db))) {
        F("[%llu] GET_MODE_DB2 failed (nv 0x%x)\n", T, g_nv.last_nv_error);
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
      nvdcHz(live), g_db.num_modes, key, have ? (p.method == PROFILE_ZOOM ? "zoom" : "mode") : "none");
    if (have && p.method == PROFILE_ZOOM) F(" (%d,%d,%d,%d)", p.zoom[0], p.zoom[1], p.zoom[2], p.zoom[3]);
    if (have && p.method == PROFILE_MODE) F(" -> %ux%u", p.mode.hActive, p.mode.vActive);
    F("\n");
}

static void tickMode(Conn *c, const NvdcMode2 *live, u64 t) {
    if (nvdcTimingEq(live, &c->profile.mode)) {
        if (!c->good_since) c->good_since = t;
        if (c->applies && t - c->good_since > STICK_MS) {
            c->applies = 0;
            c->gave_up = false;
        }
        return;
    }
    c->good_since = 0;
    if (live->hActive != c->profile.base_w || live->vActive != c->profile.base_h) return;
    if (t - c->last_change < SETTLE_MS || c->gave_up) return;
    if (c->applies >= MAX_APPLIES) {
        F("[%llu] reset %d times in a row, giving up until reconnect\n", T, c->applies);
        c->gave_up = true;
        return;
    }
    NvdcMode2 io = c->profile.mode;
    if (R_FAILED(nvsIoctl(&g_nv, g_fd, NVDISP_VALIDATE_MODE2, &io))) {
        F("[%llu] profile mode rejected by VALIDATE (nv 0x%x), giving up\n", T, g_nv.last_nv_error);
        c->gave_up = true;
        return;
    }
    io = c->profile.mode;
    Result rc = nvsIoctl(&g_nv, g_fd, NVDISP_SET_MODE2, &io);
    c->applies++;
    F("[%llu] applied %ux%u (attempt %d): rc 0x%x nv 0x%x\n", T, c->profile.mode.hActive,
      c->profile.mode.vActive, c->applies, rc, g_nv.last_nv_error);
}

static void tickZoom(Conn *c, const NvdcMode2 *live, u64 t) {
    if (live->hActive != c->profile.base_w || live->vActive != c->profile.base_h) {
        zoomReset("not base mode", false);
        return;
    }
    if (t - c->last_change < ZOOM_SETTLE_MS) return;
    if (c->zoom_sent && (c->refresh_s == 0 || t - c->zoom_sent < (u64)c->refresh_s * 1000)) return;
    if (zoomApply(c->profile.zoom)) c->zoom_sent = t;
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
        c->refresh_s = readRefresh();
        F("[%llu] disp1 active %ux%u@%.2f (zoom refresh %ds)\n", T, live.hActive, live.vActive,
          nvdcHz(&live), c->refresh_s);
    }
    if (!nvdcTimingEq(&live, &c->last)) {
        F("[%llu] live mode changed to %ux%u@%.2f\n", T, live.hActive, live.vActive, nvdcHz(&live));
        c->last = live;
        c->last_change = t;
        c->zoom_sent = 0;
        // A mode we set ourselves doesn't change the monitor; anything else might.
        if (!(c->have_profile && c->profile.method == PROFILE_MODE && nvdcTimingEq(&live, &c->profile.mode)))
            c->need_lookup = true;
    }

    bool settled = t - c->last_change >= ZOOM_SETTLE_MS;
    if ((c->need_lookup && settled) || (!c->have_profile && t - c->last_lookup >= LOOKUP_RETRY_MS))
        lookup(c, &live, t);

    if (!c->have_profile) {
        zoomReset("no profile", false);
        return;
    }
    if (c->profile.method == PROFILE_ZOOM) tickZoom(c, &live, t);
    else tickMode(c, &live, t);
}

static void openLog(void) {
    struct stat st;
    bool append = !(stat(AF_DIR "/aspectfixd.txt", &st) == 0 && st.st_size > LOG_MAX);
    logOpen(AF_DIR "/aspectfixd.txt", append);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    mkdir(AF_DIR, 0777);
    logSetConsole(false);
    openLog();
    u32 hv = hosversionGet();
    F("\n=== aspectfixd " VERSION " started, HOS %u.%u.%u ===\n", HOSVER_MAJOR(hv), HOSVER_MINOR(hv),
      HOSVER_MICRO(hv));
    if (fileExists(AF_PAUSE_FILE)) {   // nothing can be running yet at boot
        unlink(AF_PAUSE_FILE);
        F("[%llu] removed leftover pause file\n", T);
    }

    Conn c = {0};
    bool was_docked = true, was_idle = false;
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
            bool idle = isIdle();
            if (idle) {
                if (!was_idle) F("[%llu] paused/disabled\n", T);
                zoomReset("paused/disabled", false);
            } else {
                if (was_idle) {
                    F("[%llu] resumed\n", T);
                    c.connected = false;   // modetest may have saved or deleted a profile
                }
                slowTick(&c);
            }
            was_idle = idle;
        }
        svcSleepThread(FAST_MS * 1000000ULL);
    }
    return 0;
}
