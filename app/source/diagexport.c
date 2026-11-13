// Diagnostics export (Expert): the read-only checks of the prototype's diag app.
// Writes sdmc:/config/sys-scaler/diag/log.txt plus raw dumps (edid.bin, modedb2.bin).
//
// It never calls SET_MODE2 and never writes settings. The only call that hands
// the driver a timing is VALIDATE_MODE2, followed by a check that the live mode
// did not change (confirmed harmless on device, run 1).

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "diagexport.h"
#include "log.h"
#include "edid.h"
#include "aspect.h"
#include "dock.h"
#include "nvsession.h"
#include "nvdisp.h"
#include "settings.h"

#define OUT_DIR SS_DIAG_DIR
#define NV_TMEM 0x100000

static NvdcModeDB2 g_db2;   // 12 KB, keep off the stack
static EdidInfo    g_edid;
static u8          g_edid_raw[sizeof(SetSysEdid)];

static double      g_panel_ar = CONTENT_AR;
static char        g_panel_ar_name[24] = "16:9";
static const char *g_panel_ar_src = "default (no EDID)";

static void section(const char *title) {
    L("\n== %s ==\n", title);
}

static void logMode1(const char *label, const NvdcMode *m) {
    F("    %s fields: hAct=%u vAct=%u hSync=%u vSync=%u hFP=%u vFP=%u hBP=%u vBP=%u "
      "hRefToSync=%u vRefToSync=%u pclkKHz=%u bpp=%u vmode=0x%x sync=0x%x\n",
      label, m->hActive, m->vActive, m->hSyncWidth, m->vSyncWidth, m->hFrontPorch, m->vFrontPorch,
      m->hBackPorch, m->vBackPorch, m->hRefToSync, m->vRefToSync, m->pclkKHz, m->bitsPerPixel,
      m->vmode, m->sync);
}

static const char *modelName(SetSysProductModel m) {
    switch (m) {
        case SetSysProductModel_Nx:     return "Erista (original V1)";
        case SetSysProductModel_Copper: return "Copper";
        case SetSysProductModel_Iowa:   return "Mariko (V2)";
        case SetSysProductModel_Hoag:   return "Lite";
        case SetSysProductModel_Calcio: return "Calcio";
        case SetSysProductModel_Aula:   return "OLED";
        default:                        return "unknown";
    }
}

// ---------------------------------------------------------------------------
// 1. system info

static void systemInfo(void) {
    section("System");
    u32 hv = hosversionGet();
    SetSysFirmwareVersion fw = {0};
    setsysGetFirmwareVersion(&fw);
    L("  HOS %u.%u.%u (display \"%s\")%s\n", HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv),
      fw.display_version, hosversionIsAtmosphere() ? ", Atmosphere" : "");

    u64 exo = 0;
    if (R_SUCCEEDED(splInitialize())) {
        if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &exo)))
            L("  Atmosphere/Exosphere %u.%u.%u\n", (u32)(exo >> 56) & 0xFF, (u32)(exo >> 48) & 0xFF,
              (u32)(exo >> 40) & 0xFF);
        splExit();
    }

    SetSysProductModel model = SetSysProductModel_Invalid;
    setsysGetProductModel(&model);
    L("  Model: %s (%d)\n", modelName(model), model);

    AppletOperationMode op = appletGetOperationMode();
    L("  Operation mode: %s\n", op == AppletOperationMode_Console ? "DOCKED" : "handheld");
    if (op != AppletOperationMode_Console)
        L("  WARNING: not docked - disp1 / EDID results will be empty or stale.\n");
    F("  Applet type: %d\n", appletGetAppletType());

    SetSysTvSettings tv = {0};
    if (R_SUCCEEDED(setsysGetTvSettings(&tv)))
        L("  TV settings: resolution %d, underscan %u, rgb_range %d, content_type %d, flags 0x%x\n",
          tv.tv_resolution, tv.underscan, tv.rgb_range, tv.hdmi_content_type, tv.flags);
}

// ---------------------------------------------------------------------------
// 2. EDID as cached by the system (setsys GetEdid)

static void edidFromSettings(void) {
    section("EDID (set:sys GetEdid)");
    SetSysEdid e;
    memset(&e, 0, sizeof(e));
    Result rc = setsysGetEdid(&e);
    if (R_FAILED(rc)) {
        L("  setsysGetEdid failed: 0x%x\n", rc);
        return;
    }
    memcpy(g_edid_raw, &e, sizeof(e));
    size_t blocks = 1 + g_edid_raw[126];
    size_t len = blocks * 128;
    if (len > sizeof(g_edid_raw)) len = sizeof(g_edid_raw);
    writeFile(OUT_DIR "/edid.bin", g_edid_raw, len);
    logHex("edid_setsys", g_edid_raw, len);
    edidParse(g_edid_raw, len, &g_edid);
}

// Panel aspect: manual override file > EDID native resolution > EDID physical size > 16:9.
static void decidePanelAspect(void) {
    section("Panel aspect");
    g_panel_ar = panelAspect(&g_edid, NULL, &g_panel_ar_src);
    arName(g_panel_ar, g_panel_ar_name, sizeof(g_panel_ar_name));
    L("  %s (%.4f), from %s\n", g_panel_ar_name, g_panel_ar, g_panel_ar_src);
}

// ---------------------------------------------------------------------------
// 3. which nv service may open which display device

static void permissionProbe(void) {
    section("nvdrv permission probe");
    static const char *services[] = { "nvdrv", "nvdrv:a", "nvdrv:s" };
    static const char *devs[] = { "/dev/nvdisp-ctrl", "/dev/nvdisp-disp0", "/dev/nvdisp-disp1" };

    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
        NvSession s;
        Result arc = 0;
        Result rc = nvsOpen(&s, services[i], NV_TMEM, &arc);
        if (R_FAILED(rc)) {
            L("  %-8s: open/init failed 0x%x (nv err 0x%x)\n", services[i], rc, s.last_nv_error);
            continue;
        }
        L("  %-8s: init OK", services[i]);
        if (arc) F(" (SetAruid 0x%x)", arc);
        for (size_t d = 0; d < sizeof(devs) / sizeof(devs[0]); d++) {
            u32 fd = 0;
            rc = nvsDevOpen(&s, devs[d], &fd);
            L(" | %s %s", devs[d] + 5, R_SUCCEEDED(rc) ? "OK" : "denied");
            if (R_FAILED(rc)) F(" (0x%x nv 0x%x)", rc, s.last_nv_error);
            else nvsDevClose(&s, fd);
        }
        L("\n");
        nvsClose(&s);
    }
}

// ---------------------------------------------------------------------------
// 4. read-only display queries

typedef struct {
    bool      have_cur;
    NvdcMode2 cur;
    bool      have_db;
    int       db_panel_ar;      // DB modes matching the panel aspect
    DockInfo  dock;
} DispState;

static void ctrlQueries(NvSession *s) {
    section("nvdisp-ctrl");
    u32 fd = 0;
    Result rc = nvsDevOpen(s, "/dev/nvdisp-ctrl", &fd);
    if (R_FAILED(rc)) {
        L("  open failed 0x%x\n", rc);
        return;
    }
    u32 n = 0;
    rc = nvsIoctl(s, fd, NVDISP_CTRL_NUM_OUTPUTS, &n);
    L("  NUM_OUTPUTS: %s %u\n", R_SUCCEEDED(rc) ? "" : "failed", n);
    // QUERY_EDID was dropped: its input struct is undocumented (run 1 got nv 0x3000f /
    // 0x6), and set:sys GetEdid already returns the full EDID.
    nvsDevClose(s, fd);
}

static void dispQueries(NvSession *s, const char *dev, bool external, DispState *st) {
    section(dev);
    u32 fd = 0;
    Result rc = nvsDevOpen(s, dev, &fd);
    if (R_FAILED(rc)) {
        L("  open failed 0x%x (nv 0x%x)\n", rc, s->last_nv_error);
        return;
    }

    NvdcMode2 m2 = {0};
    rc = nvsIoctl(s, fd, NVDISP_GET_MODE2, &m2);
    if (R_SUCCEEDED(rc)) {
        logMode2("GET_MODE2", &m2, true);
        if (st) { st->cur = m2; st->have_cur = true; }
    } else {
        L("  GET_MODE2 failed 0x%x (nv 0x%x)\n", rc, s->last_nv_error);
    }

    NvdcMode m1 = {0};
    rc = nvsIoctl(s, fd, NVDISP_GET_MODE, &m1);
    if (R_SUCCEEDED(rc)) logMode1("GET_MODE(legacy)", &m1);
    else F("  GET_MODE(legacy) failed 0x%x (nv 0x%x)\n", rc, s->last_nv_error);

    if (!external) {
        nvsDevClose(s, fd);
        return;
    }

    memset(&g_db2, 0, sizeof(g_db2));
    rc = nvsIoctl(s, fd, NVDISP_GET_MODE_DB2, &g_db2);
    if (R_SUCCEEDED(rc)) {
        u32 n = g_db2.num_modes > 201 ? 201 : g_db2.num_modes;
        writeFile(OUT_DIR "/modedb2.bin", &g_db2, sizeof(g_db2));
        st->have_db = true;
        L("  GET_MODE_DB2: %u modes (full list in log.txt)\n", g_db2.num_modes);
        for (u32 i = 0; i < n; i++) {
            NvdcMode2 *m = &g_db2.modes[i];
            char label[24];
            snprintf(label, sizeof(label), "DB[%3u]", i);
            bool match = arMatches(m->hActive, m->vActive, g_panel_ar);
            if (match) st->db_panel_ar++;
            logMode2(label, m, match); // panel-aspect entries also go to the screen
        }
        L("  %s modes in DB: %d\n", g_panel_ar_name, st->db_panel_ar);
    } else {
        L("  GET_MODE_DB2 failed 0x%x (nv 0x%x)\n", rc, s->last_nv_error);
    }

    NvdcAviInfoframe avi = {0};
    rc = nvsIoctl(s, fd, NVDISP_GET_AVI_INFOFRAME, &avi);
    if (R_SUCCEEDED(rc)) {
        L("  AVI infoframe: VIC %u, picture aspect %u (1=4:3 2=16:9), active fmt %u, scan %u, bars %u\n",
          avi.video_format, avi.aspect_ratio, avi.act_format, avi.scan, avi.bar_valid);
        logHex("AVI infoframe (u32 fields)", &avi, sizeof(avi));
    } else {
        L("  GET_AVI_INFOFRAME failed 0x%x (nv 0x%x)\n", rc, s->last_nv_error);
    }

    // DPCD reads (DisplayPort side of the dock's DP->HDMI converter).
    dockRead(s, fd, &st->dock);
    dockLog(&st->dock);

    nvsDevClose(s, fd);
}

// ---------------------------------------------------------------------------
// 5. VALIDATE_MODE2 tests (opt-in)

typedef struct {
    char      label[48];
    NvdcMode2 m;
    bool      tried, ok;
    bool      panel_ar;   // matches the panel aspect
} Candidate;

#define MAX_CAND 40
static Candidate g_cand[MAX_CAND];
static int g_ncand;

static void addCand(const char *label, const NvdcMode2 *m) {
    if (g_ncand >= MAX_CAND) return;
    for (int i = 0; i < g_ncand; i++) {
        const NvdcMode2 *o = &g_cand[i].m;
        if (o->hActive == m->hActive && o->vActive == m->vActive && o->pclkKHz == m->pclkKHz) return;
    }
    Candidate *c = &g_cand[g_ncand++];
    memset(c, 0, sizeof(*c));
    snprintf(c->label, sizeof(c->label), "%s", label);
    c->m = *m;
    c->panel_ar = arMatches(m->hActive, m->vActive, g_panel_ar);
}

// EDID detailed timing in the shape the driver uses for its own DTD-derived DB entries.
static NvdcMode2 fromDtd(const EdidTiming *t) {
    NvdcMode2 m = {0};
    m.hActive = t->hact; m.hFrontPorch = t->hfp; m.hSyncWidth = t->hsw; m.hBackPorch = t->hbp;
    m.vActive = t->vact; m.vFrontPorch = t->vfp; m.vSyncWidth = t->vsw; m.vBackPorch = t->vbp;
    m.pclkKHz = t->pclk_khz;
    m.bitsPerPixel = 24;
    m.vmode = NVDC_VMODE_DETAILED;
    m.sync = (t->hsync_pos ? NVDC_SYNC_HPOS : 0) | (t->vsync_pos ? NVDC_SYNC_VPOS : 0);
    return m;
}

static void validateTests(NvSession *s, DispState *st) {
    section("VALIDATE_MODE2 tests");
    if (!st->have_cur) {
        L("  Skipped: no current disp1 mode.\n");
        return;
    }

    g_ncand = 0;
    addCand("control: current mode", &st->cur);
    if (st->have_db) {
        u32 n = g_db2.num_modes > 201 ? 201 : g_db2.num_modes;
        for (u32 i = 0; i < n; i++) {
            NvdcMode2 *m = &g_db2.modes[i];
            if (!arMatches(m->hActive, m->vActive, g_panel_ar)) continue;
            char label[48];
            snprintf(label, sizeof(label), "DB[%u]", i);
            addCand(label, m);
        }
    }
    for (int i = 0; i < g_edid.num_dtds; i++) {
        const EdidTiming *t = &g_edid.dtds[i];
        if (!arMatches(t->hact, t->vact, g_panel_ar) || t->interlaced) continue;
        NvdcMode2 m = fromDtd(t);
        addCand(i == 0 ? "EDID preferred DTD" : "EDID DTD", &m);
    }
    // Generated CVT-RB @60 timings in the panel aspect: the ideal size first
    // (holds 1920x1080 unscaled), then a ladder of common heights incl. native.
    u32 iw, ih;
    idealNativeMode(g_panel_ar, &iw, &ih);
    NvdcMode2 ideal = cvtRb(iw, ih, 60);
    addCand("CVT-RB ideal", &ideal);
    static const u32 heights[] = { 720, 800, 900, 1050, 1080, 1200, 1440, 1600, 1800 };
    for (size_t i = 0; i < sizeof(heights) / sizeof(heights[0]); i++) {
        u32 h = heights[i];
        u32 w = (u32)(h * g_panel_ar / 8.0 + 0.5) * 8;
        if (!arMatches(w, h, g_panel_ar)) continue;
        NvdcMode2 m = cvtRb(w, h, 60);
        addCand("CVT-RB", &m);
    }

    L("  %d candidate timings (VALIDATE_MODE2 only; the live mode is checked after).\n", g_ncand);

    u32 fd = 0;
    Result rc = nvsDevOpen(s, "/dev/nvdisp-disp1", &fd);
    if (R_FAILED(rc)) {
        L("  open disp1 failed 0x%x\n", rc);
        return;
    }
    NvdcMode2 before = {0}, after = {0};
    nvsIoctl(s, fd, NVDISP_GET_MODE2, &before);

    for (int i = 0; i < g_ncand; i++) {
        Candidate *c = &g_cand[i];
        NvdcMode2 io = c->m;
        rc = nvsIoctl(s, fd, NVDISP_VALIDATE_MODE2, &io);
        c->tried = true;
        c->ok = R_SUCCEEDED(rc);
        L("  %-18s %4ux%-4u %7.3f Hz %6u kHz -> %s", c->label, c->m.hActive, c->m.vActive,
          nvdcHz(&c->m), c->m.pclkKHz, c->ok ? "ACCEPTED" : "rejected");
        if (!c->ok) L(" (nv 0x%x)", s->last_nv_error);
        if (c->ok && st->dock.max_tmds_khz && c->m.pclkKHz > st->dock.max_tmds_khz)
            L(" but > dock %u MHz", st->dock.max_tmds_khz / 1000);
        L("\n");
        logMode2("    in ", &c->m, false);
        logMode2("    out", &io, false);
        if (memcmp(&io, &c->m, sizeof(io)) != 0) F("    (driver modified the struct)\n");
    }

    nvsIoctl(s, fd, NVDISP_GET_MODE2, &after);
    bool unchanged = nvdcTimingEq(&before, &after);
    L("  Live mode unchanged after tests: %s\n", unchanged ? "yes" : "NO - see log");
    if (!unchanged) {
        logMode2("before", &before, true);
        logMode2("after ", &after, true);
    }
    nvsDevClose(s, fd);
}

// ---------------------------------------------------------------------------
// 6. summary

static void summary(bool disp_access, const DispState *st) {
    section("Summary");
    L("  Panel aspect: %s (%s)\n", g_panel_ar_name, g_panel_ar_src);
    L("  disp1 access from homebrew: %s\n", disp_access ? "yes" : "NO");
    L("  Driver mode DB, %s modes: %s\n", g_panel_ar_name,
      st->have_db ? (st->db_panel_ar ? "yes" : "none") : "unknown");

    if (arMatches(16, 9, g_panel_ar)) {
        L("  -> Panel is 16:9, nothing to correct.\n");
        return;
    }

    // Best accepted native-aspect mode: the smallest that holds 1920x1080
    // unscaled, else the largest accepted one.
    const Candidate *best = NULL;
    bool tried = false;
    for (int i = 0; i < g_ncand; i++) {
        const Candidate *c = &g_cand[i];
        if (!c->panel_ar || !c->tried) continue;
        tried = true;
        if (!c->ok) continue;
        if (st->dock.max_tmds_khz && c->m.pclkKHz > st->dock.max_tmds_khz) continue;
        bool fits = c->m.hActive >= 1920 && c->m.vActive >= 1080;
        bool best_fits = best && best->m.hActive >= 1920 && best->m.vActive >= 1080;
        if (!best ||
            (fits && (!best_fits || c->m.vActive < best->m.vActive)) ||
            (!fits && !best_fits && c->m.vActive > best->m.vActive))
            best = c;
    }
    L("  VALIDATE in panel aspect: %s\n",
      !tried ? "not run" : best ? "at least one accepted" : "all rejected");

    // Letterbox plans for this panel.
    u32 sig_w = st->have_cur ? st->cur.hActive : 1920, sig_h = st->have_cur ? st->cur.vActive : 1080;
    if (!arMatches(sig_w, sig_h, CONTENT_AR)) { sig_w = 1920; sig_h = 1080; }
    Rect a = fitPrecompensated(g_panel_ar, sig_w, sig_h);
    L("\n  Plan A (keep %ux%u signal, monitor stretches it):\n"
      "     draw game at %ux%u, offset +%u+%u (%s bars)\n",
      sig_w, sig_h, a.w, a.h, a.x, a.y, a.y ? "top/bottom" : "left/right");

    u32 bw, bh;
    if (best) { bw = best->m.hActive; bh = best->m.vActive; }
    else      idealNativeMode(g_panel_ar, &bw, &bh);
    Rect b = fitNative(bw, bh);
    L("  Plan B (output %ux%u%s):\n"
      "     game at %ux%u, offset +%u+%u%s\n",
      bw, bh, best ? ", driver accepted" : ", untested", b.w, b.h, b.x, b.y,
      (b.w == 1920 && b.h == 1080) ? " (no scaling)" : " (scaled)");

    if (!disp_access)
        L("\n  -> Could not reach nvdisp from an .nro. Next: same probes as a sysmodule.\n");
    else if (best)
        L("\n  -> Plan B looks possible. Next: SET_MODE2 test with auto-revert.\n");
    else
        L("\n  -> No native-aspect mode accepted%s. Plan A is the fallback.\n",
          tried ? "" : " (tests not run)");
}

// ---------------------------------------------------------------------------

bool diagExport(void) {
    mkdir(OUT_DIR, 0777);
    if (!logOpen(OUT_DIR "/log.txt", false)) return false;
    L(SS_NAME " " SS_VERSION " diagnostics\n");
    systemInfo();
    edidFromSettings();
    decidePanelAspect();
    permissionProbe();

    DispState st = {0};
    bool disp_access = false;
    NvSession s;
    Result rc = nvsOpen(&s, "nvdrv:s", NV_TMEM, NULL);
    if (R_SUCCEEDED(rc)) {
        ctrlQueries(&s);
        dispQueries(&s, "/dev/nvdisp-disp0", false, NULL);
        dispQueries(&s, "/dev/nvdisp-disp1", true, &st);
        disp_access = st.have_cur;
        validateTests(&s, &st);
        nvsClose(&s);
    } else {
        L("\nnvdrv:s unavailable (0x%x); skipping display queries.\n", rc);
    }
    summary(disp_access, &st);
    L("\nDone.\n");
    logClose();
    return true;
}
