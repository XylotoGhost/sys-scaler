// AspectFix ModeTest: try the two ways of showing 16:9 correctly on a non-16:9
// monitor, confirm one, and save it as a per-monitor profile that the aspectfixd
// sysmodule applies automatically.
//
//  CENTERED (zoom): keep the stock 1920x1080 output; vi's display magnification
//    pre-squashes the image (16:10 -> 1920x972 at y+54) and the monitor's own
//    stretch makes it 16:9 again, with bars top and bottom. Slightly softer.
//  SHARP (mode): switch the output to a native-aspect mode (1920x1200); vi draws
//    the image 1:1, top-aligned, bar at the bottom only (vi clips at 1080, run 5).
//
// Safety:
//  - Modes: only >= 1920x1080 (vi does not scale to the output, smaller modes
//    crop), within the dock's max TMDS clock, from the driver DB or CVT-RB, and
//    only if VALIDATE_MODE2 accepts them. 1080p restore mode validated first.
//  - Every change has a 15 s countdown; no X press -> automatic revert.
//  - A profile is only written after that confirmation. In handheld mode the app
//    offers recovery (delete all profiles, reset zoom, toggle aspectfixd).
//  - While this app runs it creates sdmc:/aspectfix/pause so aspectfixd stays out
//    of the way.
//
// Log: sdmc:/aspectfix/modetest.txt (appended per run)

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "log.h"
#include "edid.h"
#include "aspect.h"
#include "dock.h"
#include "profile.h"
#include "vimag.h"
#include "nvsession.h"
#include "nvdisp.h"

#define APP_VERSION     "0.4.1"
#define NV_TMEM         0x100000
#define CONFIRM_SECONDS 15
#define POLL_MS         250

typedef struct {
    char        label[20];
    bool        zoom;       // the CENTERED entry
    NvdcMode2   m;
    const char *unusable;   // NULL = selectable, else reason
} Cand;

static PadState    g_pad;
static NvSession   g_nv;
static u32         g_fd;
static NvdcModeDB2 g_db;
static u32         g_key;
static EdidInfo    g_edid;
static DockInfo    g_dock;
static double      g_ar;
static char        g_arname[24];
static NvdcMode2   g_restore;
static Cand        g_cand[24];
static int         g_ncand, g_sel;

// ---------------------------------------------------------------------------

static u64 buttonsDown(void) {
    padUpdate(&g_pad);
    return padGetButtonsDown(&g_pad);
}

static u64 waitButtons(u64 mask) {
    while (appletMainLoop()) {
        u64 k = buttonsDown();
        if (k & mask) return k;
        consoleUpdate(NULL);
    }
    return 0;
}

static u64 nowMs(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

static bool getMode(NvdcMode2 *m) {
    memset(m, 0, sizeof(*m));
    return R_SUCCEEDED(nvsIoctl(&g_nv, g_fd, NVDISP_GET_MODE2, m));
}

static Result validateMode(const NvdcMode2 *m) {
    NvdcMode2 io = *m;
    return nvsIoctl(&g_nv, g_fd, NVDISP_VALIDATE_MODE2, &io);
}

static Result setMode(const NvdcMode2 *m) {
    NvdcMode2 io = *m;
    Result rc = nvsIoctl(&g_nv, g_fd, NVDISP_SET_MODE2, &io);
    F("[%llu ms] SET_MODE2 %ux%u %u kHz -> rc 0x%x nv 0x%x\n", (unsigned long long)nowMs(),
      m->hActive, m->vActive, m->pclkKHz, rc, g_nv.last_nv_error);
    return rc;
}

static bool isStock(const NvdcMode2 *m) {
    return nvdcTimingEq(m, &g_restore);
}

static void logAvi(const char *when) {
    NvdcAviInfoframe avi = {0};
    if (R_SUCCEEDED(nvsIoctl(&g_nv, g_fd, NVDISP_GET_AVI_INFOFRAME, &avi)))
        F("  AVI (%s): VIC %u aspect %u act_fmt %u scan %u bars %u\n", when,
          avi.video_format, avi.aspect_ratio, avi.act_format, avi.scan, avi.bar_valid);
}

// Resets vi magnification to the full logical display. Used before tests and
// in the handheld recovery menu.
static Result zoomResetNow(void) {
    ViMag vm;
    Result rc = vimagOpen(&vm, NULL);
    if (R_FAILED(rc)) return rc;
    s32 lw = 1920, lh = 1080;
    vimagGetLogicalResolution(&vm, &lw, &lh);
    rc = vimagSet(&vm, 0, 0, lw, lh);
    F("Zoom reset to (0,0,%d,%d): rc 0x%x\n", lw, lh, rc);
    vimagClose(&vm);
    return rc;
}

static void appShutdown(void) {
    unlink(AF_PAUSE_FILE);   // let aspectfixd resume
    F("Exit.\n");
    logClose();
    if (g_nv.active) {
        nvsDevClose(&g_nv, g_fd);
        nvsClose(&g_nv);
    }
    setsysExit();
    consoleExit(NULL);
}

// Countdown with X to confirm. Optionally polls that the live mode stays `expect`.
static bool confirmCountdown(u64 t0, const NvdcMode2 *expect) {
    bool reported = false;
    u64 last_poll = 0;
    while (appletMainLoop()) {
        u64 el = nowMs() - t0;
        if (el >= CONFIRM_SECONDS * 1000ULL) return false;
        if (buttonsDown() & HidNpadButton_X) return true;
        if (expect && el - last_poll >= POLL_MS) {
            last_poll = el;
            NvdcMode2 cur;
            if (getMode(&cur) && !nvdcTimingEq(&cur, expect) && !reported) {
                reported = true;
                L("\n  !! Mode changed by the system after %llu ms -> %ux%u\n",
                  (unsigned long long)el, cur.hActive, cur.vActive);
            }
        }
        printf("\r  Reverting in %2llu s ...   ", (unsigned long long)(CONFIRM_SECONDS - el / 1000));
        consoleUpdate(NULL);
    }
    return false;
}

// ---------------------------------------------------------------------------
// setup

static void findRestoreMode(void) {
    // Stock docked mode: CEA VIC 16, 1920x1080 @ 60 Hz, 148.5 MHz. Prefer the
    // driver's own DB entry so bpp/vmode/aspect flags are exactly what it built.
    u32 n = g_db.num_modes > 201 ? 201 : g_db.num_modes;
    for (u32 i = 0; i < n; i++) {
        const NvdcMode2 *m = &g_db.modes[i];
        if (m->hActive == 1920 && m->vActive == 1080 && m->pclkKHz == 148500 &&
            m->hFrontPorch == 88 && (m->vmode & NVDC_VMODE_CEA)) {
            g_restore = *m;
            F("Restore mode = DB[%u]\n", i);
            return;
        }
    }
    NvdcMode2 m = {0};
    m.hActive = 1920; m.hFrontPorch = 88; m.hSyncWidth = 44; m.hBackPorch = 148;
    m.vActive = 1080; m.vFrontPorch = 4;  m.vSyncWidth = 5;  m.vBackPorch = 36;
    m.pclkKHz = 148500; m.bitsPerPixel = 24; m.vmode = NVDC_VMODE_CEA;
    m.sync = NVDC_SYNC_HPOS | NVDC_SYNC_VPOS; m.unk1 = NVDC_ASPECT_16_9;
    g_restore = m;
    F("Restore mode = built-in CEA 1080p60\n");
}

static void addCand(const char *label, const NvdcMode2 *m) {
    if (g_ncand >= (int)(sizeof(g_cand) / sizeof(g_cand[0]))) return;
    for (int i = 0; i < g_ncand; i++)
        if (!g_cand[i].zoom && g_cand[i].m.hActive == m->hActive && g_cand[i].m.vActive == m->vActive &&
            nvdcHz(&g_cand[i].m) > nvdcHz(m) - 0.5 && nvdcHz(&g_cand[i].m) < nvdcHz(m) + 0.5)
            return;
    Cand *c = &g_cand[g_ncand++];
    memset(c, 0, sizeof(*c));
    snprintf(c->label, sizeof(c->label), "%s", label);
    c->m = *m;
    if (m->hActive < 1920 || m->vActive < 1080)
        c->unusable = "smaller than 1920x1080, crops";
    else if (g_dock.max_tmds_khz && m->pclkKHz > g_dock.max_tmds_khz)
        c->unusable = "over dock limit";
}

static void buildCandidates(void) {
    // Entry 0: CENTERED (no mode change, always usable).
    Cand *z = &g_cand[g_ncand++];
    memset(z, 0, sizeof(*z));
    snprintf(z->label, sizeof(z->label), "CENTERED");
    z->zoom = true;
    z->m = g_restore;

    u32 iw, ih;
    idealNativeMode(g_ar, &iw, &ih);
    u32 n = g_db.num_modes > 201 ? 201 : g_db.num_modes;
    for (u32 i = 0; i < n; i++) {
        const NvdcMode2 *m = &g_db.modes[i];
        if (!arMatches(m->hActive, m->vActive, g_ar)) continue;
        char label[20];
        snprintf(label, sizeof(label), "SHARP DB[%u]", i);
        addCand(label, m);
    }
    NvdcMode2 ideal = cvtRb(iw, ih, 60);
    addCand("SHARP CVT-RB", &ideal);   // skipped if the DB already has it
    g_sel = 0;
}

// ---------------------------------------------------------------------------
// tests

static const char *askResult(void) {
    L("\n  How does it look?\n"
      "    Up    = good: proportions right, bars where expected\n"
      "    A     = no visible change\n"
      "    B     = squashed / stretched / off-center\n"
      "    Y     = cut off, black, or otherwise broken\n");
    u64 k = waitButtons(HidNpadButton_AnyUp | HidNpadButton_A | HidNpadButton_B | HidNpadButton_Y);
    const char *a = (k & HidNpadButton_AnyUp) ? "good" : (k & HidNpadButton_A) ? "no change" :
                    (k & HidNpadButton_B) ? "squashed/stretched/off-center" : "cut off / broken";
    L("  -> answer: %s\n", a);
    return a;
}

static void runZoomTest(void) {
    consoleClear();
    NvdcMode2 cur;
    if (getMode(&cur) && !isStock(&cur)) {
        L("  Switching back to stock 1080p first.\n");
        setMode(&g_restore);
        svcSleepThread(1500000000ULL);
    }

    ViMag vm;
    Result first_rc = 0;
    Result rc = vimagOpen(&vm, &first_rc);
    if (R_FAILED(rc)) {
        L("  vi:m session / OpenDisplay failed 0x%x (first 0x%x). Press B.\n", rc, first_rc);
        waitButtons(HidNpadButton_B);
        return;
    }
    s32 lw = 1920, lh = 1080, z[4];
    vimagGetLogicalResolution(&vm, &lw, &lh);
    zoomRectForPanel(g_ar, lw, lh, z);
    Rect r = fitPrecompensated(g_ar, (u32)lw, (u32)lh);
    L("\n== Test CENTERED: stock %dx%d + zoom (%d,%d,%d,%d) ==\n", lw, lh, z[0], z[1], z[2], z[3]);
    L("  Image drawn at %ux%u +%u+%u; your monitor stretches it back to 16:9.\n", r.w, r.h, r.x, r.y);

    appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    rc = vimagSet(&vm, z[0], z[1], z[2], z[3]);
    F("  SetDisplayMagnification -> rc 0x%x\n", rc);
    if (R_FAILED(rc)) {
        L("  vi rejected it (0x%x). Press B.\n", rc);
        waitButtons(HidNpadButton_B);
        vimagClose(&vm);
        appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
        return;
    }

    L("  If you can read this, press X within %d s (press HOME to look at real content;\n"
      "  the timer keeps running).\n\n", CONFIRM_SECONDS);
    if (!confirmCountdown(nowMs(), NULL)) {
        vimagSet(&vm, 0, 0, lw, lh);
        L("\n  Reverted (no confirmation). Press B.\n");
        waitButtons(HidNpadButton_B);
        vimagClose(&vm);
        appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
        return;
    }
    askResult();

    L("\n  Y = SAVE as profile \"centered\" for this monitor (aspectfixd applies it)\n"
      "  + = undo and go back to the list\n");
    u64 k = waitButtons(HidNpadButton_Y | HidNpadButton_Plus);
    if (k & HidNpadButton_Y) {
        Profile p;
        memset(&p, 0, sizeof(p));
        p.method = PROFILE_ZOOM;
        p.base_w = (u32)lw;
        p.base_h = (u32)lh;
        memcpy(p.zoom, z, sizeof(z));
        bool ok = profileSave(g_key, &p, g_edid.name);
        L("  Profile %08X (centered) %s. aspectfixd takes over when you exit this app.\n",
          g_key, ok ? "saved" : "COULD NOT be saved");
    } else {
        vimagSet(&vm, 0, 0, lw, lh);
        L("  Undone.\n");
    }
    vimagClose(&vm);
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
    L("  Press B.\n");
    waitButtons(HidNpadButton_B);
}

// Returns true if the app should exit (mode kept).
static bool runModeTest(const Cand *c) {
    consoleClear();
    L("\n== Test %s %ux%u @ %.3f Hz, %u kHz ==\n", c->label, c->m.hActive, c->m.vActive,
      nvdcHz(&c->m), c->m.pclkKHz);
    logMode2("test mode", &c->m, false);

    Result rc = validateMode(&c->m);
    if (R_FAILED(rc)) {
        L("  VALIDATE_MODE2 rejected it (nv 0x%x). Nothing changed. Press B.\n", g_nv.last_nv_error);
        waitButtons(HidNpadButton_B);
        return false;
    }

    appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    logAvi("before");
    u64 t0 = nowMs();
    rc = setMode(&c->m);
    if (R_FAILED(rc)) {
        L("  SET_MODE2 failed (rc 0x%x nv 0x%x).\n", rc, g_nv.last_nv_error);
        NvdcMode2 cur;
        if (getMode(&cur) && !isStock(&cur)) setMode(&g_restore);
        L("  Press B.\n");
        waitButtons(HidNpadButton_B);
        appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
        return false;
    }

    L("  Mode set. If you can read this on the monitor, press X within %d s.\n"
      "  Otherwise it reverts to 1080p by itself.\n\n", CONFIRM_SECONDS);
    if (!confirmCountdown(t0, &c->m)) {
        setMode(&g_restore);
        NvdcMode2 cur;
        getMode(&cur);
        L("\n  Reverted (no confirmation). Live mode now %ux%u. Press B.\n", cur.hActive, cur.vActive);
        waitButtons(HidNpadButton_B);
        appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
        return false;
    }

    L("\n  Confirmed visible after %llu ms.\n", (unsigned long long)(nowMs() - t0));
    NvdcMode2 cur;
    if (getMode(&cur)) logMode2("live", &cur, true);
    logAvi("after");

    L("\n  Y = SAVE as profile \"sharp\" for this monitor (aspectfixd applies it)\n"
      "  + = restore 1080p and go back to the list\n"
      "  - = keep this mode for now (no profile) and exit\n");
    bool exit_app = false;
    u64 k = waitButtons(HidNpadButton_Y | HidNpadButton_Plus | HidNpadButton_Minus);
    if (k & HidNpadButton_Y) {
        Profile p;
        memset(&p, 0, sizeof(p));
        p.method = PROFILE_MODE;
        p.base_w = g_restore.hActive;
        p.base_h = g_restore.vActive;
        p.mode = c->m;
        bool ok = profileSave(g_key, &p, g_edid.name);
        L("  Profile %08X (sharp) %s.\n", g_key, ok ? "saved" : "COULD NOT be saved");
    } else if (k & HidNpadButton_Plus) {
        setMode(&g_restore);
        getMode(&cur);
        L("  Restored, live mode %ux%u.\n", cur.hActive, cur.vActive);
    } else {
        L("  Keeping %ux%u without a profile. Exiting.\n", c->m.hActive, c->m.vActive);
        exit_app = true;
    }
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
    if (!exit_app) {
        L("  Press B.\n");
        waitButtons(HidNpadButton_B);
    }
    return exit_app;
}

// ---------------------------------------------------------------------------
// menu

static void drawMenu(void) {
    consoleClear();
    NvdcMode2 cur;
    bool have = getMode(&cur);
    Profile prof;
    bool have_prof = profileLoad(g_key, &prof);
    printf("AspectFix ModeTest " APP_VERSION "\n\n");
    printf("Monitor: \"%s\", panel %s.  Dock max TMDS: ", g_edid.name, g_arname);
    if (g_dock.max_tmds_khz) printf("%u MHz\n", g_dock.max_tmds_khz / 1000); else printf("unknown\n");
    if (have)
        printf("Live mode: %ux%u @ %.2f Hz%s\n", cur.hActive, cur.vActive, nvdcHz(&cur),
               isStock(&cur) ? " (stock)" : "  <-- not stock, R restores 1080p");
    printf("Profile %08X: ", g_key);
    if (!have_prof) printf("none\n");
    else if (prof.method == PROFILE_ZOOM)
        printf("CENTERED, zoom (%d,%d,%d,%d)\n", prof.zoom[0], prof.zoom[1], prof.zoom[2], prof.zoom[3]);
    else printf("SHARP, %ux%u\n", prof.mode.hActive, prof.mode.vActive);
    printf("aspectfixd: %s\n\n", fileExists(AF_DISABLE_FILE) ? "DISABLED  <-- ZR to enable" :
                                 "enabled (paused while this app runs)");

    printf("  CENTERED = stock 1080p, bars top+bottom, slightly softer\n");
    printf("  SHARP    = native-aspect output, pixel-sharp, bar at the bottom only\n\n");
    for (int i = 0; i < g_ncand; i++) {
        const Cand *c = &g_cand[i];
        if (c->zoom) {
            printf(" %s %-14s 1920x1080 + vi zoom\n", i == g_sel ? ">" : " ", c->label);
            continue;
        }
        printf(" %s %-14s %4ux%-4u %7.3f Hz %7.2f MHz%s%s%s\n", i == g_sel ? ">" : " ", c->label,
               c->m.hActive, c->m.vActive, nvdcHz(&c->m), c->m.pclkKHz / 1000.0,
               c->unusable ? "  (" : "", c->unusable ? c->unusable : "", c->unusable ? ")" : "");
    }
    printf("\nUp/Down select  A test  R restore 1080p  L delete profile  ZR on/off  + exit\n");
    consoleUpdate(NULL);
}

static void menu(void) {
    drawMenu();
    while (appletMainLoop()) {
        u64 k = buttonsDown();
        if (k & HidNpadButton_Plus) break;
        if ((k & (HidNpadButton_AnyUp | HidNpadButton_AnyDown)) && g_ncand) {
            int dir = (k & HidNpadButton_AnyUp) ? -1 : 1;
            for (int i = 0; i < g_ncand; i++) {
                int nxt = (g_sel + dir * (i + 1) + g_ncand * 4) % g_ncand;
                if (!g_cand[nxt].unusable) { g_sel = nxt; break; }
            }
            drawMenu();
        }
        if ((k & HidNpadButton_A) && g_sel >= 0) {
            if (g_cand[g_sel].zoom) runZoomTest();
            else if (runModeTest(&g_cand[g_sel])) return;
            drawMenu();
        }
        if (k & HidNpadButton_R) {
            setMode(&g_restore);
            zoomResetNow();
            L("Restore requested from menu.\n");
            drawMenu();
        }
        if (k & HidNpadButton_L) {
            F("Delete profile %08X: %s\n", g_key, profileDelete(g_key) ? "ok" : "none");
            drawMenu();
        }
        if (k & HidNpadButton_ZR) {
            if (fileExists(AF_DISABLE_FILE)) unlink(AF_DISABLE_FILE);
            else touchFile(AF_DISABLE_FILE);
            F("aspectfixd %s\n", fileExists(AF_DISABLE_FILE) ? "disabled" : "enabled");
            drawMenu();
        }
        consoleUpdate(NULL);
    }
}

// Handheld: nothing to test, but offer recovery in case a profile misbehaves.
static void handheldMenu(void) {
    L("Not docked. Dock the Switch with the monitor attached to test.\n\n"
      "Recovery:\n"
      "  Y  = delete ALL profiles\n"
      "  X  = reset zoom (if the screen looks squashed)\n"
      "  ZR = toggle aspectfixd (now %s)\n"
      "  +  = exit\n", fileExists(AF_DISABLE_FILE) ? "disabled" : "enabled");
    while (appletMainLoop()) {
        u64 k = buttonsDown();
        if (k & HidNpadButton_Plus) break;
        if (k & HidNpadButton_Y) L("Deleted %d profile(s).\n", profileDeleteAll());
        if (k & HidNpadButton_X) L("Zoom reset: rc 0x%x\n", zoomResetNow());
        if (k & HidNpadButton_ZR) {
            if (fileExists(AF_DISABLE_FILE)) unlink(AF_DISABLE_FILE);
            else touchFile(AF_DISABLE_FILE);
            L("aspectfixd now %s.\n", fileExists(AF_DISABLE_FILE) ? "disabled" : "enabled");
        }
        consoleUpdate(NULL);
    }
}

static bool setup(void) {
    SetSysEdid e;
    memset(&e, 0, sizeof(e));
    if (R_SUCCEEDED(setsysGetEdid(&e))) {
        const u8 *raw = (const u8 *)&e;
        size_t len = (1 + raw[126]) * 128;
        if (len > sizeof(e)) len = sizeof(e);
        edidParse(raw, len, &g_edid);
    }
    const char *src;
    g_ar = panelAspect(&g_edid, AF_DIR "/aspect.txt", &src);
    arName(g_ar, g_arname, sizeof(g_arname));
    L("Panel aspect %s from %s\n", g_arname, src);

    Result rc = nvsOpen(&g_nv, "nvdrv:s", NV_TMEM, NULL);
    if (R_FAILED(rc)) { L("nvdrv:s unavailable (0x%x)\n", rc); return false; }
    rc = nvsDevOpen(&g_nv, "/dev/nvdisp-disp1", &g_fd);
    if (R_FAILED(rc)) { L("open disp1 failed (0x%x)\n", rc); return false; }

    NvdcMode2 cur;
    if (getMode(&cur)) logMode2("start mode", &cur, true);
    rc = nvsIoctl(&g_nv, g_fd, NVDISP_GET_MODE_DB2, &g_db);
    if (R_FAILED(rc)) { L("GET_MODE_DB2 failed (nv 0x%x)\n", g_nv.last_nv_error); g_db.num_modes = 0; }
    g_key = profileKey(&g_db);
    F("Mode DB key %08X (%u modes)\n", g_key, g_db.num_modes);
    dockRead(&g_nv, g_fd, &g_dock);
    dockLog(&g_dock);

    findRestoreMode();
    logMode2("restore mode", &g_restore, false);
    if (R_FAILED(validateMode(&g_restore))) {
        L("The driver rejects the 1080p restore mode (nv 0x%x). Not testing anything.\n",
          g_nv.last_nv_error);
        return false;
    }
    if (arMatches(16, 9, g_ar)) {
        L("Panel is 16:9, nothing to test.\n");
        return false;
    }
    buildCandidates();
    return true;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    mkdir(AF_DIR, 0777);
    logOpen(AF_DIR "/modetest.txt", true);
    F("\n\n######## AspectFix ModeTest " APP_VERSION " ########\n");
    u32 hv = hosversionGet();
    F("HOS %u.%u.%u\n", HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv));
    setsysInitialize();

    if (appletGetOperationMode() != AppletOperationMode_Console) {
        handheldMenu();
        appShutdown();
        return 0;
    }

    pauseWrite();
    if (setup()) {
        menu();
    } else {
        L("\nPress + to exit.\n");
        waitButtons(HidNpadButton_Plus);
    }
    appShutdown();
    return 0;
}
