// sys-scaler app: set up and manage the docked aspect-ratio fix.
//
// Easy mode: one guided setup (test circle -> centered fix with a 15 s safety
// countdown -> "is it round?") plus on/off. Expert mode adds method and monitor
// shape choice, fine-tuning, the re-send interval, service control, the list of
// saved monitors and a diagnostics export.
//
// Safety: every change to the picture is shown with a 15 s countdown and reverts
// unless A is pressed; nothing is saved without confirmation; fine-tuning keeps
// the picture fully on screen. While a test runs the app holds the pause file so
// the background service stays out of the way. Handheld mode is never affected.
//
// Log: sdmc:/config/sys-scaler/app-log.txt

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "ui.h"
#include "hw.h"
#include "diagexport.h"
#include "log.h"
#include "settings.h"

#define CONFIRM_MS   15000
#define PREP_MS      1300     // let the service notice the pause file (1 s tick)
#define STATUS_MS    1000
#define APP_LOG_MAX  (64 * 1024)

// colours (dark theme)
#define C_BG      RGB(45, 45, 45)
#define C_PANEL   RGB(56, 56, 56)
#define C_SEP     RGB(77, 77, 77)
#define C_TEXT    RGB(255, 255, 255)
#define C_DIM     RGB(160, 160, 160)
#define C_SEL_BG  RGB(36, 36, 40)
#define C_SEL     RGB(0, 198, 232)
#define C_GOOD    RGB(60, 212, 138)
#define C_WARN    RGB(245, 166, 35)
#define C_BAD     RGB(255, 90, 90)
#define C_CARD    RGB(30, 30, 30)
#define C_PAT_BG  RGB(18, 18, 18)

typedef enum { SCR_MAIN, SCR_PREP, SCR_ASK, SCR_TEST, SCR_JUDGE, SCR_FINETUNE, SCR_MONITORS, SCR_MSG,
               SCR_CONFIRM } Screen;
typedef enum { IT_SETUP, IT_METHOD, IT_SHAPE, IT_TEST, IT_FINETUNE, IT_ENABLED, IT_REFRESH, IT_BOOT,
               IT_SERVICE, IT_MONITORS, IT_FORGET, IT_RESETZOOM, IT_DELETEALL, IT_DIAG, IT_MODE } ItemId;
typedef enum { PREP_ASK, PREP_TEST, PREP_FINETUNE } PrepNext;

typedef struct {
    ItemId      id;
    char        label[48];
    char        value[48];
    bool        adjustable;
    u32         value_col;
    const char *desc;
} Item;

static PadState    g_pad;
static PadRepeater g_rep;
static u64         g_down, g_rpt;
static Settings    g_set;
static Screen      g_scr = SCR_MAIN;
static bool        g_quit;
static bool        g_docked;
static int         g_sel, g_scroll;

// status, refreshed every second
static u64       g_status_at;
static bool      g_have_prof;
static Profile   g_prof;
static bool      g_svc_running, g_svc_installed, g_boot;
static int       g_prof_count;   // saved monitors
static NvdcMode2 g_live;
static bool      g_have_live;

// Expert draft
static int       g_draft_method = PROFILE_CENTERED;
static int       g_draft_shape;   // index into g_shapes, 0 = auto
static bool      g_sharp_ok;
static NvdcMode2 g_sharp_mode;

// test / wizard state
static bool     g_testing;        // pause file held, focus = no suspend
static bool     g_wizard;         // Easy guided setup (step labels)
static Profile  g_test;           // candidate being tested
static u64      g_t0;
static PrepNext g_prep_next;
static Rect     g_ft;             // fine-tune image rect (reference units)
static s32      g_ft_before[4];
static bool     g_ft_from_judge;

// message / confirm
static char   g_msg_title[64], g_msg_body[512];
static u32    g_msg_col;
static char   g_conf_title[96], g_conf_body[256];
static void (*g_conf_yes)(void);
static u32    g_conf_key;
static Screen g_conf_back;

// saved monitors
static u32     g_mon_keys[PROFILE_MAX];
static Profile g_mon[PROFILE_MAX];
static int     g_mon_n, g_mon_sel;

static u64 nowMs(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

// ---------------------------------------------------------------------------
// state helpers

static void refreshStatus(bool force) {
    u64 t = nowMs();
    if (!force && t - g_status_at < STATUS_MS) return;
    g_status_at = t;
    g_have_prof = g_hw.ready && profileLoad(g_hw.key, &g_prof);
    g_svc_running = sysmodRunning();
    g_svc_installed = sysmodInstalled();
    g_boot = bootFlagGet();
    g_have_live = g_hw.ready && hwLiveMode(&g_live);
    u32 keys[PROFILE_MAX];
    g_prof_count = profileList(keys, PROFILE_MAX);
}

static void updateSharp(void) {
    g_sharp_ok = hwFindSharp(shapeAr(g_draft_shape), &g_sharp_mode);
    if (!g_sharp_ok && g_draft_method == PROFILE_SHARP) g_draft_method = PROFILE_CENTERED;
}

static void draftFromProfile(void) {
    g_draft_method = PROFILE_CENTERED;
    g_draft_shape = 0;
    if (g_have_prof) {
        g_draft_method = g_prof.method;
        if (g_prof.ar_w && g_prof.ar_h &&
            !arMatches(g_prof.ar_w, g_prof.ar_h, g_hw.ar_auto))
            g_draft_shape = shapeFind(g_prof.ar_w, g_prof.ar_h);
    }
    updateSharp();
}

static void saveSettings(void) {
    if (!settingsSave(&g_set)) F("settings save failed\n");
}

static void msg(const char *title, u32 col, const char *body) {
    snprintf(g_msg_title, sizeof(g_msg_title), "%s", title);
    snprintf(g_msg_body, sizeof(g_msg_body), "%s", body);
    g_msg_col = col;
    g_scr = SCR_MSG;
}

static void confirm(const char *title, const char *body, void (*yes)(void)) {
    snprintf(g_conf_title, sizeof(g_conf_title), "%s", title);
    snprintf(g_conf_body, sizeof(g_conf_body), "%s", body);
    g_conf_yes = yes;
    g_conf_back = g_scr;
    g_scr = SCR_CONFIRM;
}

static void loadMonitors(void) {
    g_mon_n = profileList(g_mon_keys, PROFILE_MAX);
    for (int i = 0; i < g_mon_n; i++)
        if (!profileLoad(g_mon_keys[i], &g_mon[i])) {
            memset(&g_mon[i], 0, sizeof(g_mon[i]));
            g_mon[i].method = PROFILE_OFF;
            snprintf(g_mon[i].name, sizeof(g_mon[i].name), "(unreadable)");
        }
    if (g_mon_sel >= g_mon_n) g_mon_sel = g_mon_n ? g_mon_n - 1 : 0;
}

static Profile newProfile(ProfileMethod m, double ar) {
    Profile p;
    memset(&p, 0, sizeof(p));
    p.method = m;
    arToFraction(ar, &p.ar_w, &p.ar_h);
    snprintf(p.name, sizeof(p.name), "%s", g_hw.edid.name[0] ? g_hw.edid.name : "Unknown monitor");
    return p;
}

// ---------------------------------------------------------------------------
// tests

static void enterTestMode(void) {
    if (g_testing) return;
    g_testing = true;
    pauseWrite();
    appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    F("test mode on\n");
}

static void leaveTestMode(void) {
    if (!g_testing) return;
    g_testing = false;
    pauseRemoveIfOurs();
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
    refreshStatus(true);
    F("test mode off\n");
}

// Puts the picture back to plain stock output (the service re-applies a saved
// profile once the test mode ends).
static void revertPicture(void) {
    hwRestoreStock();
    hwZoomReset();
}

static void startPrep(PrepNext next) {
    enterTestMode();
    g_prep_next = next;
    g_t0 = nowMs();
    g_scr = SCR_PREP;
}

static bool applyTest(void) {
    Result rc;
    if (g_test.method == PROFILE_SHARP) {
        hwZoomReset();
        rc = hwSetMode(&g_test.mode);
    } else {
        hwRestoreStock();
        rc = hwZoomSet(g_test.zoom);
    }
    F("apply test %s: rc 0x%x\n", profileMethodName(g_test.method), rc);
    if (R_FAILED(rc)) {
        revertPicture();
        leaveTestMode();
        char b[160];
        snprintf(b, sizeof(b), "The system refused this setting (error 0x%x). Nothing was changed.", rc);
        msg("Couldn't apply it", C_BAD, b);
        return false;
    }
    g_t0 = nowMs();
    g_scr = SCR_TEST;
    return true;
}

static void startWizard(void) {
    g_wizard = true;
    g_test = newProfile(PROFILE_CENTERED, g_hw.ar_auto);
    zoomRectForPanel(g_hw.ar_auto, 1920, 1080, g_test.zoom);
    F("wizard: panel %s, zoom (%d,%d,%d,%d)\n", g_hw.ar_auto_name, g_test.zoom[0], g_test.zoom[1],
      g_test.zoom[2], g_test.zoom[3]);
    startPrep(PREP_ASK);
}

static void saveTested(void) {
    bool ok = profileSave(g_hw.key, &g_test);
    F("save profile %08X %s: %s\n", g_hw.key, profileMethodName(g_test.method), ok ? "ok" : "FAILED");
    leaveTestMode();
    saveSettings();   // bump rev so the service re-reads at once
    refreshStatus(true);
    draftFromProfile();
    g_wizard = false;
    if (!ok) { msg("Couldn't save", C_BAD, "Writing to the SD card failed. Nothing was saved."); return; }
    if (g_test.method == PROFILE_OFF)
        msg("Nothing to fix", C_GOOD, "This monitor already shows the picture correctly, so sys-scaler will "
            "leave it alone. You can close the app.");
    else
        msg("All set", C_GOOD, "sys-scaler now fixes the picture whenever this monitor is connected, and turns "
            "itself off when you undock. You can close the app.");
}

static void testExpert(void) {
    g_wizard = false;
    double ar = shapeAr(g_draft_shape);
    g_test = newProfile((ProfileMethod)g_draft_method, ar);
    if (g_draft_method == PROFILE_OFF) {
        saveTested();
        return;
    }
    if (g_draft_method == PROFILE_SHARP) {
        if (!g_sharp_ok || !g_hw.stock_ok) {
            msg("Not available", C_WARN, "No sharp mode fits this monitor shape and dock.");
            return;
        }
        g_test.mode = g_sharp_mode;
    } else {
        zoomRectForPanel(ar, 1920, 1080, g_test.zoom);
    }
    startPrep(PREP_TEST);
}

static void testUndone(bool timeout) {
    revertPicture();
    leaveTestMode();
    g_wizard = false;
    if (timeout)
        msg("Reverted", C_WARN, "No confirmation within 15 seconds, so the picture went back to how it was. "
            "Nothing was saved.");
    else
        msg("Undone", C_DIM, "Nothing was saved. If the picture was close but not quite right, Expert mode "
            "lets you choose the monitor shape or fine-tune size and position.");
}

static void ftApply(void) {
    zoomFromImage(&g_ft, g_test.zoom);
    hwZoomSet(g_test.zoom);
}

static void startFinetuneFromMain(void) {
    g_wizard = false;
    g_test = g_prof;
    g_ft_from_judge = false;
    startPrep(PREP_FINETUNE);
}

// ---------------------------------------------------------------------------
// actions

static void doForget(void) {
    bool ok = profileDelete(g_conf_key);
    F("delete profile %08X: %s\n", g_conf_key, ok ? "ok" : "failed");
    saveSettings();
    refreshStatus(true);
    draftFromProfile();
    loadMonitors();
    g_scr = g_mon_n && g_conf_key != g_hw.key ? SCR_MONITORS : SCR_MAIN;
    if (g_conf_key == g_hw.key) hwZoomReset();
}

static void doDeleteAll(void) {
    int n = profileDeleteAll();
    F("delete all profiles: %d\n", n);
    saveSettings();
    refreshStatus(true);
    hwZoomReset();
    char b[96];
    snprintf(b, sizeof(b), "Removed %d saved monitor%s.", n, n == 1 ? "" : "s");
    msg("Done", C_GOOD, b);
}

static void doRestartService(void) {
    if (g_svc_running) {
        sysmodStop();
        svcSleepThread(300000000ULL);
    }
    Result rc = sysmodStart();
    svcSleepThread(300000000ULL);
    refreshStatus(true);
    if (R_FAILED(rc) && !g_svc_running) {
        char b[128];
        snprintf(b, sizeof(b), "Starting the background service failed (0x%x). Reboot to start it.", rc);
        msg("Service", C_BAD, b);
    }
}

static void doDiag(void) {
    // one frame so the user sees something while it runs (a few seconds)
    uiBegin(C_BG);
    uiTextCenter(UI_W / 2, 330, 30, C_TEXT, "Exporting diagnostics…");
    uiEnd();
    logClose();
    bool ok = diagExport();
    logOpen(SS_APP_LOG, true);
    F("diagnostics export: %s\n", ok ? "ok" : "failed");
    if (ok) msg("Diagnostics saved", C_GOOD, "Saved to sdmc:/config/sys-scaler/diag/ (log.txt, edid.bin, "
                "modedb2.bin). Attach these when reporting a problem.");
    else msg("Export failed", C_BAD, "Couldn't write to the SD card.");
}

static const int g_refresh_opts[] = { 0, 1, 2, 3, 5, 10 };
#define NUM_REFRESH ((int)(sizeof(g_refresh_opts) / sizeof(g_refresh_opts[0])))

static void adjust(ItemId id, int dir) {
    switch (id) {
    case IT_METHOD: {
        for (int i = 0; i < 3; i++) {
            g_draft_method = (g_draft_method + dir + 3) % 3;
            if (g_draft_method != PROFILE_SHARP || g_sharp_ok) break;
        }
        break;
    }
    case IT_SHAPE:
        g_draft_shape = (g_draft_shape + dir + g_num_shapes) % g_num_shapes;
        updateSharp();
        break;
    case IT_ENABLED:
        g_set.enabled = !g_set.enabled;
        saveSettings();
        if (!g_set.enabled) hwZoomReset();
        break;
    case IT_REFRESH: {
        int cur = 0;
        for (int i = 0; i < NUM_REFRESH; i++) if (g_refresh_opts[i] == g_set.refresh_s) cur = i;
        cur = (cur + dir + NUM_REFRESH) % NUM_REFRESH;
        g_set.refresh_s = g_refresh_opts[cur];
        saveSettings();
        break;
    }
    case IT_BOOT:
        bootFlagSet(!g_boot);
        refreshStatus(true);
        break;
    case IT_MODE:
        g_set.expert = !g_set.expert;
        saveSettings();
        g_sel = 0;
        g_scroll = 0;
        break;
    default:
        break;
    }
}

static void activate(ItemId id) {
    bool need_hw = id == IT_SETUP || id == IT_TEST || id == IT_FINETUNE || id == IT_FORGET;
    if (need_hw && !g_hw.ready) {
        msg("Display not available", C_BAD, g_hw.error);
        return;
    }
    switch (id) {
    case IT_SETUP:    startWizard(); break;
    case IT_TEST:     testExpert(); break;
    case IT_FINETUNE: startFinetuneFromMain(); break;
    case IT_SERVICE:
        if (!g_svc_installed) msg("Not installed", C_WARN, "Copy the atmosphere folder from the release to "
                                  "your SD card, then reboot.");
        else doRestartService();
        break;
    case IT_MONITORS:
        loadMonitors();
        g_mon_sel = 0;
        g_scr = SCR_MONITORS;
        break;
    case IT_FORGET:
        g_conf_key = g_hw.key;
        confirm("Forget this monitor?", "Its saved setting is deleted and the picture goes back to normal.",
                doForget);
        break;
    case IT_RESETZOOM:
        hwZoomReset();
        msg("Done", C_GOOD, "The zoom was reset. If the picture still looks wrong, reboot the console.");
        break;
    case IT_DELETEALL:
        confirm("Delete all saved monitors?", "Every monitor goes back to the normal, unfixed picture.",
                doDeleteAll);
        break;
    case IT_DIAG:     doDiag(); break;
    default:          adjust(id, 1); break;
    }
}

// ---------------------------------------------------------------------------
// main list

static int addItem(Item *it, int n, ItemId id, const char *label, const char *value, bool adj, u32 col,
                   const char *desc) {
    Item *i = &it[n];
    i->id = id;
    snprintf(i->label, sizeof(i->label), "%s", label);
    snprintf(i->value, sizeof(i->value), "%s", value ? value : "");
    i->adjustable = adj;
    i->value_col = col;
    i->desc = desc;
    return n + 1;
}

static int buildItems(Item *it) {
    int n = 0;
    char v[48];
    bool ex = g_set.expert;
    if (g_docked) {
        n = addItem(it, n, IT_SETUP, g_have_prof ? "Set up again" : "Set up this monitor", NULL, false, 0,
                    "Shows a test circle and walks you through fixing the picture. Every change reverts "
                    "by itself unless you confirm it.");
        if (ex) {
            n = addItem(it, n, IT_METHOD, "Method", profileMethodName(g_draft_method), true, C_SEL,
                        g_draft_method == PROFILE_CENTERED ?
                        "Centered: keeps the normal signal and squeezes the picture so the monitor's stretch "
                        "makes it right. Bars on both sides, works on any monitor, slightly softer." :
                        g_draft_method == PROFILE_SHARP ?
                        "Sharp: sends a signal in the monitor's own shape. Pixel-sharp, but the bar is only "
                        "at the bottom. Only for monitors taller than 16:9." :
                        "Off: leave this monitor alone.");
            if (g_draft_shape == 0) snprintf(v, sizeof(v), "Auto (%s)", g_hw.ar_auto_name);
            else snprintf(v, sizeof(v), "%s", g_shapes[g_draft_shape].name);
            n = addItem(it, n, IT_SHAPE, "Monitor shape", v, true, C_SEL,
                        "Auto reads the shape from the monitor. Change it if the circle isn't round after "
                        "setup, e.g. when the monitor reports the wrong resolution.");
            n = addItem(it, n, IT_TEST, "Test and save", NULL, false, 0,
                        "Tries the method and shape above, with the 15-second safety countdown.");
            if (g_have_prof && g_prof.method == PROFILE_CENTERED)
                n = addItem(it, n, IT_FINETUNE, "Fine-tune picture", NULL, false, 0,
                            "Move and resize the picture with live preview, e.g. if the monitor cuts off an "
                            "edge.");
        }
    }
    n = addItem(it, n, IT_ENABLED, "Picture fix", g_set.enabled ? "On" : "Off", true,
                g_set.enabled ? C_GOOD : C_WARN, "Turns sys-scaler on or off for all monitors.");
    if (!g_docked)
        n = addItem(it, n, IT_RESETZOOM, "Reset picture now", NULL, false, 0,
                    "Use this if the built-in screen ever looks squeezed.");
    if (ex) {
        if (g_set.refresh_s) snprintf(v, sizeof(v), "%d s", g_set.refresh_s);
        else snprintf(v, sizeof(v), "Off");
        n = addItem(it, n, IT_REFRESH, "Re-send interval", v, true, C_SEL,
                    "How often the centered fix is re-applied. The system resets it after booting and in "
                    "the power menu; 1 s repairs that almost instantly.");
        n = addItem(it, n, IT_BOOT, "Start at boot", g_boot ? "On" : "Off", true, g_boot ? C_GOOD : C_WARN,
                    "Starts the background service when the console boots (Atmosphère boot2 flag).");
        n = addItem(it, n, IT_SERVICE, "Background service",
                    !g_svc_installed ? "Not installed" : g_svc_running ? "Running" : "Stopped", false,
                    g_svc_running ? C_GOOD : C_BAD,
                    "Press A to start the service now, or to restart it after an update.");
        snprintf(v, sizeof(v), "%d", g_prof_count);
        n = addItem(it, n, IT_MONITORS, "Saved monitors", v, false, C_DIM,
                    "Every monitor you set up is remembered separately. Delete ones you no longer use.");
    }
    if (g_docked && g_have_prof)
        n = addItem(it, n, IT_FORGET, "Forget this monitor", NULL, false, 0,
                    "Deletes this monitor's saved setting.");
    if (!g_docked)
        n = addItem(it, n, IT_DELETEALL, "Delete all saved monitors", NULL, false, 0,
                    "Every monitor goes back to the normal picture.");
    if (ex && g_docked)
        n = addItem(it, n, IT_DIAG, "Export diagnostics", NULL, false, 0,
                    "Writes monitor and dock details to the SD card for bug reports.");
    n = addItem(it, n, IT_MODE, "Expert mode", ex ? "On" : "Off", true, ex ? C_SEL : C_DIM,
                "More options: method, monitor shape, fine-tuning, re-send interval, service control, "
                "saved monitors and diagnostics.");
    return n;
}

// ---------------------------------------------------------------------------
// drawing

static void drawHeader(const char *right) {
    int w = uiText(60, 30, 32, C_TEXT, SS_NAME);
    uiText(60 + w + 14, 42, 20, C_DIM, "v" SS_VERSION);
    if (right) uiTextRight(1220, 40, 22, C_DIM, right);
    uiFill(30, 88, 1220, 1, C_SEP);
}

static void drawFooter(const char *hints) {
    uiFill(30, 647, 1220, 1, C_SEP);
    uiTextRight(1220, 666, 22, C_TEXT, hints);
}

static void statusDot(int x, int y, u32 col, const char *txt, int size) {
    uiDisc(x + 7, y + size / 2 + 1, 7, col);
    uiText(x + 24, y, size, C_TEXT, txt);
}

static void drawStatusPanel(void) {
    uiRoundRect(30, 108, 380, 520, 12, C_PANEL);
    int x = 54, y = 128;
    char b[96];
    if (!g_docked) {
        uiText(x, y, 18, C_DIM, "Console");
        uiText(x, y + 24, 28, C_TEXT, "Handheld");
        uiTextWrap(x, y + 64, 330, 20, 26, C_DIM,
                   "Dock the Switch with your monitor connected to set it up. The fix is never active in "
                   "handheld mode.");
        y += 190;
    } else {
        uiText(x, y, 18, C_DIM, "Monitor");
        uiText(x, y + 24, 28, C_TEXT, g_hw.edid.name[0] ? g_hw.edid.name : "Unknown monitor");
        if (g_hw.edid.native_w)
            snprintf(b, sizeof(b), "%s · %u × %u", g_hw.ar_auto_name, g_hw.edid.native_w, g_hw.edid.native_h);
        else
            snprintf(b, sizeof(b), "%s", g_hw.ar_auto_name);
        uiText(x, y + 62, 20, C_DIM, b);
        y += 108;
        uiText(x, y, 18, C_DIM, "Output");
        if (g_have_live) snprintf(b, sizeof(b), "%u × %u · %.0f Hz", g_live.hActive, g_live.vActive, nvdcHz(&g_live));
        else snprintf(b, sizeof(b), "—");
        uiText(x, y + 24, 22, C_TEXT, b);
        y += 72;
    }

    uiText(x, y, 18, C_DIM, "Picture fix");
    const char *st, *detail = NULL;
    u32 col;
    char sb[64];
    if (!g_set.enabled) { st = "Turned off"; col = C_WARN; detail = "Turn on \"Picture fix\" to use it."; }
    else if (!g_docked) {
        st = "On"; col = C_GOOD;
        snprintf(sb, sizeof(sb), "%d saved monitor%s", g_prof_count, g_prof_count == 1 ? "" : "s");
        detail = sb;
    }
    else if (!g_hw.ready) { st = "Unavailable"; col = C_BAD; detail = g_hw.error; }
    else if (!g_have_prof) { st = "Not set up"; col = C_DIM; detail = "Choose \"Set up this monitor\"."; }
    else if (g_prof.method == PROFILE_OFF) { st = "Not needed"; col = C_GOOD; detail = "This monitor shows 16:9 correctly by itself."; }
    else if (g_prof.method == PROFILE_CENTERED) {
        st = "Centered"; col = C_GOOD;
        Rect r = zoomToImage(g_prof.zoom);
        detail = r.w < 1920 ? "Bars left and right" : "Bars top and bottom";
    } else {
        snprintf(sb, sizeof(sb), "Sharp · %u × %u", g_prof.mode.hActive, g_prof.mode.vActive);
        st = sb; col = C_GOOD; detail = "Bar at the bottom";
    }
    statusDot(x, y + 24, col, st, 24);
    if (detail) uiTextWrap(x, y + 58, 330, 18, 24, C_DIM, detail);
    y += 120;

    uiText(x, y, 18, C_DIM, "Background service");
    if (!g_svc_installed) statusDot(x, y + 24, C_BAD, "Not installed", 22);
    else if (g_svc_running) statusDot(x, y + 24, C_GOOD, "Running", 22);
    else statusDot(x, y + 24, C_BAD, g_boot ? "Stopped (reboot)" : "Stopped", 22);
}

#define LIST_X    450
#define LIST_W    800
#define LIST_Y    108
#define ROW_H     62
#define LIST_ROWS 7

static void drawRow(int y, const char *label, const char *value, u32 vcol, bool adj, bool sel) {
    if (sel) {
        uiRoundRect(LIST_X - 6, y, LIST_W + 12, ROW_H - 4, 8, C_SEL);
        uiRoundRect(LIST_X - 2, y + 4, LIST_W + 4, ROW_H - 12, 6, C_SEL_BG);
    } else {
        uiFill(LIST_X, y + ROW_H - 3, LIST_W, 1, C_SEP);
    }
    uiText(LIST_X + 20, y + 15, 24, C_TEXT, label);
    if (value && value[0]) {
        int right = LIST_X + LIST_W - 20;
        if (adj && sel) {
            uiArrow(right - 10, y + 21, 16, true, C_SEL);
            right -= 22;
        }
        int w = uiTextRight(right, y + 15, 24, vcol ? vcol : C_TEXT, value);
        if (adj && sel) uiArrow(right - w - 22, y + 21, 16, false, C_SEL);
    }
}

static void drawMain(void) {
    Item it[20];
    int n = buildItems(it);
    if (g_sel >= n) g_sel = n - 1;
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + LIST_ROWS) g_scroll = g_sel - LIST_ROWS + 1;

    uiBegin(C_BG);
    drawHeader(g_set.expert ? "Expert mode" : "Easy mode");
    drawStatusPanel();
    for (int i = g_scroll; i < n && i < g_scroll + LIST_ROWS; i++)
        drawRow(LIST_Y + (i - g_scroll) * ROW_H, it[i].label, it[i].value, it[i].value_col, it[i].adjustable,
                i == g_sel);
    if (n > LIST_ROWS) {   // scrollbar
        int track = LIST_ROWS * ROW_H - 8, th = track * LIST_ROWS / n;
        int ty = LIST_Y + (track - th) * g_scroll / (n - LIST_ROWS);
        uiFill(LIST_X + LIST_W + 16, LIST_Y, 4, track, C_SEP);
        uiFill(LIST_X + LIST_W + 16, ty, 4, th, C_DIM);
    }
    if (it[g_sel].desc) uiTextWrap(LIST_X + 20, 560, LIST_W - 40, 20, 26, C_DIM, it[g_sel].desc);
    drawFooter(it[g_sel].adjustable ? G_DPAD " Change   " G_A " Select   " G_B " Exit"
                                    : G_A " Select   " G_B " Exit");
    uiEnd();
}

static void updateMain(void) {
    Item it[20];
    int n = buildItems(it);
    if (g_sel >= n) g_sel = n - 1;
    if (g_rpt & HidNpadButton_AnyUp) g_sel = (g_sel + n - 1) % n;
    if (g_rpt & HidNpadButton_AnyDown) g_sel = (g_sel + 1) % n;
    if (it[g_sel].adjustable) {
        if (g_rpt & HidNpadButton_AnyLeft) adjust(it[g_sel].id, -1);
        if (g_rpt & HidNpadButton_AnyRight) adjust(it[g_sel].id, 1);
    }
    if (g_down & HidNpadButton_A) activate(it[g_sel].id);
    else if (g_down & (HidNpadButton_B | HidNpadButton_Plus)) g_quit = true;
}

// Test pattern: what a 16:9 picture looks like. On a correctly fixed monitor the
// circles are round and the frame shows all four edges.
static void drawPattern(void) {
    uiBegin(C_PAT_BG);
    uiFrame(0, 0, UI_W, UI_H, 6, C_TEXT);
    for (int x = 160; x < UI_W; x += 160) uiFill(x, 0, 1, UI_H, RGB(50, 50, 50));
    for (int y = 120; y < UI_H; y += 120) uiFill(0, y, UI_W, 1, RGB(50, 50, 50));
    uiRing(640, 250, 200, 8, C_TEXT);
    uiFill(632, 250, 16, 2, C_TEXT);
    uiFill(639, 243, 2, 16, C_TEXT);
    static const int cx[] = { 110, 1170, 110, 1170 }, cy[] = { 110, 110, 610, 610 };
    for (int i = 0; i < 4; i++) uiRing(cx[i], cy[i], 60, 5, C_SEL);
}

static void card(int y, int h) {
    uiRoundRect(220, y, 840, h, 16, C_CARD);
}

static void drawPrep(void) {
    drawPattern();
    card(500, 120);
    uiTextCenter(640, 540, 26, C_TEXT, "Preparing…");
    uiEnd();
}

static void drawAsk(void) {
    drawPattern();
    card(470, 220);
    uiText(260, 490, 18, C_DIM, "STEP 1 OF 2");
    uiText(260, 514, 28, C_TEXT, "Is the big circle round?");
    uiText(260, 566, 22, C_TEXT, G_A "  Yes, it's round: my monitor shows it correctly");
    uiText(260, 602, 22, C_TEXT, G_B "  No, it's stretched or squeezed: fix it");
    uiText(260, 638, 22, C_DIM, G_PLUS "  Cancel");
    uiEnd();
}

static void drawTest(void) {
    drawPattern();
    card(470, 220);
    u64 el = nowMs() - g_t0;
    int left = el >= CONFIRM_MS ? 0 : (int)((CONFIRM_MS - el + 999) / 1000);
    char b[64];
    if (g_wizard) uiText(260, 490, 18, C_DIM, "STEP 2 OF 2");
    else {
        snprintf(b, sizeof(b), "TESTING: %s", g_test.method == PROFILE_SHARP ? "SHARP" : "CENTERED");
        uiText(260, 490, 18, C_DIM, b);
    }
    uiText(260, 514, 28, C_TEXT, "Can you read this?");
    uiText(260, 566, 22, C_TEXT, G_A "  Yes, keep going");
    uiText(260, 602, 22, C_DIM, G_B "  Undo now");
    snprintf(b, sizeof(b), "%d", left);
    uiTextRight(1010, 520, 72, C_SEL, b);
    uiTextRight(1010, 610, 18, C_DIM, "seconds until it reverts");
    uiEnd();
}

static void drawJudge(void) {
    drawPattern();
    card(470, 220);
    uiText(260, 490, 18, C_DIM, g_wizard ? "STEP 2 OF 2" : "CHECK");
    uiText(260, 514, 28, C_TEXT, "Is the big circle round now?");
    uiText(260, 566, 22, C_TEXT, G_A "  Yes, save it");
    if (g_test.method == PROFILE_CENTERED)
        uiText(260, 602, 22, C_TEXT, G_X "  Almost: fine-tune size and position");
    uiText(260, 638, 22, C_DIM, G_B "  No, undo");
    uiEnd();
}

static void drawFinetune(void) {
    drawPattern();
    card(500, 190);
    char b[96];
    uiText(260, 516, 28, C_TEXT, "Fine-tune");
    snprintf(b, sizeof(b), "Picture %u × %u at %u, %u", g_ft.w, g_ft.h, g_ft.x, g_ft.y);
    uiTextRight(1020, 524, 20, C_DIM, b);
    uiText(260, 566, 22, C_TEXT, G_DPAD " Move     " G_L " " G_R " Width     " G_ZL " " G_ZR " Height");
    uiText(260, 604, 22, C_TEXT, G_X " Automatic     " G_A " Done     " G_B " Cancel");
    uiText(260, 642, 18, C_DIM, "Hold a button to move faster.");
    uiEnd();
}

static void drawMonitors(void) {
    uiBegin(C_BG);
    drawHeader("Saved monitors");
    if (!g_mon_n) uiText(LIST_X - 390, 140, 24, C_DIM, "No saved monitors.");
    int first = g_mon_sel >= 8 ? g_mon_sel - 7 : 0;
    for (int i = first; i < g_mon_n && i < first + 8; i++) {
        const Profile *p = &g_mon[i];
        int y = LIST_Y + (i - first) * ROW_H;
        bool sel = i == g_mon_sel;
        char label[80], value[64];
        snprintf(label, sizeof(label), "%s%s", p->name[0] ? p->name : "Unknown monitor",
                 g_docked && g_mon_keys[i] == g_hw.key ? "  (connected)" : "");
        if (p->ar_w && p->ar_h) snprintf(value, sizeof(value), "%s · %u:%u", profileMethodName(p->method), p->ar_w, p->ar_h);
        else snprintf(value, sizeof(value), "%s", profileMethodName(p->method));
        // full-width rows on this screen
        if (sel) {
            uiRoundRect(54, y, 1172, ROW_H - 4, 8, C_SEL);
            uiRoundRect(58, y + 4, 1164, ROW_H - 12, 6, C_SEL_BG);
        } else {
            uiFill(60, y + ROW_H - 3, 1160, 1, C_SEP);
        }
        uiText(80, y + 15, 24, C_TEXT, label);
        char key[16];
        snprintf(key, sizeof(key), "%08X", g_mon_keys[i]);
        uiText(620, y + 19, 18, C_DIM, key);
        uiTextRight(1200, y + 15, 24, C_TEXT, value);
    }
    drawFooter(g_mon_n ? G_A " Delete   " G_B " Back" : G_B " Back");
    uiEnd();
}

static void drawMsg(void) {
    uiBegin(C_BG);
    drawHeader(NULL);
    uiDisc(80, 180, 12, g_msg_col);
    uiText(110, 160, 36, C_TEXT, g_msg_title);
    uiTextWrap(110, 230, 1000, 24, 34, C_TEXT, g_msg_body);
    drawFooter(G_A " OK");
    uiEnd();
}

static void drawConfirm(void) {
    uiBegin(C_BG);
    drawHeader(NULL);
    uiDisc(80, 180, 12, C_WARN);
    uiText(110, 160, 36, C_TEXT, g_conf_title);
    uiTextWrap(110, 230, 1000, 24, 34, C_TEXT, g_conf_body);
    drawFooter(G_A " Yes   " G_B " No");
    uiEnd();
}

// ---------------------------------------------------------------------------

static void ftClamp(void) {
    if (g_ft.w < 800) g_ft.w = 800;
    if (g_ft.w > 1920) g_ft.w = 1920;
    if (g_ft.h < 450) g_ft.h = 450;
    if (g_ft.h > 1080) g_ft.h = 1080;
    if ((int)g_ft.x < 0 || g_ft.x > 1920 - g_ft.w) g_ft.x = (int)g_ft.x < 0 ? 0 : 1920 - g_ft.w;
    if ((int)g_ft.y < 0 || g_ft.y > 1080 - g_ft.h) g_ft.y = (int)g_ft.y < 0 ? 0 : 1080 - g_ft.h;
}

static void updateFinetune(void) {
    int step = (g_rpt & ~g_down) ? 6 : 2;   // first press fine, auto-repeat faster
    int x = g_ft.x, y = g_ft.y, w = g_ft.w, h = g_ft.h;
    bool changed = false;
    if (g_rpt & HidNpadButton_AnyLeft)  { x -= step; changed = true; }
    if (g_rpt & HidNpadButton_AnyRight) { x += step; changed = true; }
    if (g_rpt & HidNpadButton_AnyUp)    { y -= step; changed = true; }
    if (g_rpt & HidNpadButton_AnyDown)  { y += step; changed = true; }
    if (g_rpt & HidNpadButton_L)  { w -= 2 * step; x += step; changed = true; }
    if (g_rpt & HidNpadButton_R)  { w += 2 * step; x -= step; changed = true; }
    if (g_rpt & HidNpadButton_ZL) { h -= 2 * step; y += step; changed = true; }
    if (g_rpt & HidNpadButton_ZR) { h += 2 * step; y -= step; changed = true; }
    if (changed) {
        g_ft.x = x < 0 ? 0 : x; g_ft.y = y < 0 ? 0 : y;
        g_ft.w = w < 0 ? 0 : w; g_ft.h = h < 0 ? 0 : h;
        ftClamp();
        ftApply();
    }
    if (g_down & HidNpadButton_X) {
        double ar = g_test.ar_w && g_test.ar_h ? (double)g_test.ar_w / g_test.ar_h : g_hw.ar_auto;
        g_ft = fitPrecompensated(ar, 1920, 1080);
        ftApply();
    }
    if (g_down & HidNpadButton_A) {
        F("fine-tune done: image %ux%u+%u+%u zoom (%d,%d,%d,%d)\n", g_ft.w, g_ft.h, g_ft.x, g_ft.y,
          g_test.zoom[0], g_test.zoom[1], g_test.zoom[2], g_test.zoom[3]);
        if (g_ft_from_judge) g_scr = SCR_JUDGE;
        else saveTested();
    } else if (g_down & HidNpadButton_B) {
        memcpy(g_test.zoom, g_ft_before, sizeof(g_ft_before));
        hwZoomSet(g_test.zoom);
        if (g_ft_from_judge) g_scr = SCR_JUDGE;
        else { leaveTestMode(); g_scr = SCR_MAIN; }
    }
}

static void enterFinetune(bool from_judge) {
    g_ft_from_judge = from_judge;
    memcpy(g_ft_before, g_test.zoom, sizeof(g_ft_before));
    g_ft = zoomToImage(g_test.zoom);
    ftClamp();
    ftApply();
    g_scr = SCR_FINETUNE;
}

static void update(void) {
    switch (g_scr) {
    case SCR_MAIN:
        updateMain();
        break;
    case SCR_PREP:
        if (g_svc_running && nowMs() - g_t0 < PREP_MS) break;
        if (g_prep_next == PREP_ASK) { revertPicture(); g_scr = SCR_ASK; }
        else if (g_prep_next == PREP_TEST) applyTest();
        else enterFinetune(false);
        break;
    case SCR_ASK:
        if (g_down & HidNpadButton_A) {
            g_test = newProfile(PROFILE_OFF, g_hw.ar_auto);
            saveTested();
        } else if (g_down & HidNpadButton_B) {
            applyTest();
        } else if (g_down & HidNpadButton_Plus) {
            leaveTestMode();
            g_wizard = false;
            g_scr = SCR_MAIN;
        }
        break;
    case SCR_TEST:
        if (g_down & HidNpadButton_A) g_scr = SCR_JUDGE;
        else if (g_down & HidNpadButton_B) testUndone(false);
        else if (nowMs() - g_t0 >= CONFIRM_MS) testUndone(true);
        break;
    case SCR_JUDGE:
        if (g_down & HidNpadButton_A) saveTested();
        else if ((g_down & HidNpadButton_X) && g_test.method == PROFILE_CENTERED) enterFinetune(true);
        else if (g_down & HidNpadButton_B) testUndone(false);
        break;
    case SCR_FINETUNE:
        updateFinetune();
        break;
    case SCR_MONITORS:
        if (g_mon_n && (g_rpt & HidNpadButton_AnyUp)) g_mon_sel = (g_mon_sel + g_mon_n - 1) % g_mon_n;
        if (g_mon_n && (g_rpt & HidNpadButton_AnyDown)) g_mon_sel = (g_mon_sel + 1) % g_mon_n;
        if (g_mon_n && (g_down & HidNpadButton_A)) {
            g_conf_key = g_mon_keys[g_mon_sel];
            char b[96];
            snprintf(b, sizeof(b), "Forget \"%s\"?", g_mon[g_mon_sel].name[0] ? g_mon[g_mon_sel].name : "Unknown monitor");
            confirm(b, "Its saved setting is deleted. Set it up again any time.", doForget);
        } else if (g_down & HidNpadButton_B) {
            g_scr = SCR_MAIN;
        }
        break;
    case SCR_MSG:
        if (g_down & (HidNpadButton_A | HidNpadButton_B)) g_scr = SCR_MAIN;
        break;
    case SCR_CONFIRM:
        if (g_down & HidNpadButton_A) g_conf_yes();
        else if (g_down & HidNpadButton_B) g_scr = g_conf_back;
        break;
    }
}

static void draw(void) {
    switch (g_scr) {
    case SCR_MAIN:     drawMain(); break;
    case SCR_PREP:     drawPrep(); break;
    case SCR_ASK:      drawAsk(); break;
    case SCR_TEST:     drawTest(); break;
    case SCR_JUDGE:    drawJudge(); break;
    case SCR_FINETUNE: drawFinetune(); break;
    case SCR_MONITORS: drawMonitors(); break;
    case SCR_MSG:      drawMsg(); break;
    case SCR_CONFIRM:  drawConfirm(); break;
    }
}

// Docking or undocking while the app is open: abort any test, re-read hardware.
static void checkDock(void) {
    bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    if (docked == g_docked) return;
    F("operation mode changed: %s\n", docked ? "docked" : "handheld");
    bool was_testing = g_testing;
    if (g_testing) {
        if (!docked) hwZoomReset();
        leaveTestMode();
    }
    g_wizard = false;
    g_docked = docked;
    hwInit(docked);
    refreshStatus(true);
    draftFromProfile();
    g_sel = 0;
    g_scroll = 0;
    if (was_testing) msg("Test cancelled", C_WARN, "The console was docked or undocked. Nothing was saved.");
    else if (g_scr != SCR_MSG && g_scr != SCR_CONFIRM) g_scr = SCR_MAIN;
}

static void openAppLog(void) {
    struct stat st;
    bool append = !(stat(SS_APP_LOG, &st) == 0 && st.st_size > APP_LOG_MAX);
    logOpen(SS_APP_LOG, append);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    ssMkdirs();
    logSetConsole(false);
    openAppLog();
    u32 hv = hosversionGet();
    F("\n=== " SS_NAME " app " SS_VERSION ", HOS %u.%u.%u ===\n", HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv));

    if (!uiInit()) {
        F("UI init failed\n");
        uiExit();
        logClose();
        return 1;
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    padRepeaterInitialize(&g_rep, 20, 4);
    setsysInitialize();
    pmdmntInitialize();
    pmshellInitialize();

    int migrated = settingsMigrateLegacy();
    if (migrated >= 0) F("first start: config.ini created, %d prototype profile(s) taken over\n", migrated);
    settingsLoad(&g_set);

    g_docked = appletGetOperationMode() == AppletOperationMode_Console;
    hwInit(g_docked);
    refreshStatus(true);
    draftFromProfile();

    while (appletMainLoop() && !g_quit) {
        padUpdate(&g_pad);
        g_down = padGetButtonsDown(&g_pad);
        padRepeaterUpdate(&g_rep, padGetButtons(&g_pad) &
                          (HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft |
                           HidNpadButton_AnyRight | HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL |
                           HidNpadButton_ZR));
        g_rpt = g_down | padRepeaterGetButtons(&g_rep);
        checkDock();
        if (!g_testing) refreshStatus(false);
        update();
        if (!g_quit) draw();
    }

    if (g_testing) {
        revertPicture();
        leaveTestMode();
    }
    pauseRemoveIfOurs();
    F("exit\n");
    hwExit();
    pmshellExit();
    pmdmntExit();
    setsysExit();
    uiExit();
    logClose();
    return 0;
}
