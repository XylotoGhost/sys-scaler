#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "hw.h"
#include "log.h"
#include "settings.h"
#include "vimag.h"

#define NV_TMEM 0x100000

Hw g_hw;
static NvSession g_nv;
static u32       g_fd;
static bool      g_fd_open;
static ViMag     g_vm;

const Shape g_shapes[] = {
    { 0, 0, "Auto" },
    { 16, 10, "16:10" },
    { 3, 2, "3:2" },
    { 4, 3, "4:3" },
    { 5, 4, "5:4" },
    { 64, 27, "21:9 (2560 × 1080)" },
    { 43, 18, "21:9 (3440 × 1440)" },
    { 32, 9, "32:9" },
    { 16, 9, "16:9 (no fix)" },
};
const int g_num_shapes = sizeof(g_shapes) / sizeof(g_shapes[0]);

double shapeAr(int idx) {
    if (idx <= 0 || idx >= g_num_shapes) return g_hw.ar_auto;
    return (double)g_shapes[idx].w / g_shapes[idx].h;
}

int shapeFind(u32 w, u32 h) {
    for (int i = 1; i < g_num_shapes; i++)
        if (g_shapes[i].w * h == g_shapes[i].h * w) return i;
    return 0;
}

void arToFraction(double ar, u32 *w, u32 *h) {
    for (int i = 1; i < g_num_shapes; i++)
        if (fabs(ar / ((double)g_shapes[i].w / g_shapes[i].h) - 1) < 0.01) {
            *w = g_shapes[i].w;
            *h = g_shapes[i].h;
            return;
        }
    *w = (u32)lround(ar * 1000);
    *h = 1000;
}

// ---------------------------------------------------------------------------

static void findStock(void) {
    u32 n = g_hw.db.num_modes > 201 ? 201 : g_hw.db.num_modes;
    for (u32 i = 0; i < n; i++) {
        const NvdcMode2 *m = &g_hw.db.modes[i];
        if (m->hActive == 1920 && m->vActive == 1080 && m->pclkKHz == 148500 &&
            m->hFrontPorch == 88 && (m->vmode & NVDC_VMODE_CEA)) {
            g_hw.stock = *m;
            return;
        }
    }
    NvdcMode2 m = {0};
    m.hActive = 1920; m.hFrontPorch = 88; m.hSyncWidth = 44; m.hBackPorch = 148;
    m.vActive = 1080; m.vFrontPorch = 4;  m.vSyncWidth = 5;  m.vBackPorch = 36;
    m.pclkKHz = 148500; m.bitsPerPixel = 24; m.vmode = NVDC_VMODE_CEA;
    m.sync = NVDC_SYNC_HPOS | NVDC_SYNC_VPOS; m.unk1 = NVDC_ASPECT_16_9;
    g_hw.stock = m;
}

void hwInit(bool docked) {
    hwExit();
    memset(&g_hw, 0, sizeof(g_hw));
    g_hw.docked = docked;
    g_hw.ar_auto = CONTENT_AR;
    snprintf(g_hw.ar_auto_name, sizeof(g_hw.ar_auto_name), "16:9");
    if (!docked) {
        snprintf(g_hw.error, sizeof(g_hw.error), "Not docked");
        return;
    }

    SetSysEdid e;
    memset(&e, 0, sizeof(e));
    if (R_SUCCEEDED(setsysGetEdid(&e))) {
        const u8 *raw = (const u8 *)&e;
        size_t len = (1 + raw[126]) * 128;
        if (len > sizeof(e)) len = sizeof(e);
        edidParse(raw, len, &g_hw.edid);
        g_hw.have_edid = g_hw.edid.num_blocks > 0;
    }
    const char *src;
    g_hw.ar_auto = panelAspect(&g_hw.edid, NULL, &src);
    arName(g_hw.ar_auto, g_hw.ar_auto_name, sizeof(g_hw.ar_auto_name));
    F("Monitor \"%s\", panel %s from %s\n", g_hw.edid.name, g_hw.ar_auto_name, src);

    Result rc = nvsOpen(&g_nv, "nvdrv:s", NV_TMEM, NULL);
    if (R_FAILED(rc)) { snprintf(g_hw.error, sizeof(g_hw.error), "nvdrv:s unavailable (0x%x)", rc); return; }
    rc = nvsDevOpen(&g_nv, "/dev/nvdisp-disp1", &g_fd);
    if (R_FAILED(rc)) { snprintf(g_hw.error, sizeof(g_hw.error), "Display access failed (0x%x)", rc); return; }
    g_fd_open = true;
    if (R_FAILED(nvsIoctl(&g_nv, g_fd, NVDISP_GET_MODE_DB2, &g_hw.db))) {
        snprintf(g_hw.error, sizeof(g_hw.error), "Can't read the monitor's modes (nv 0x%x)", g_nv.last_nv_error);
        return;
    }
    g_hw.key = profileKey(&g_hw.db);
    dockRead(&g_nv, g_fd, &g_hw.dock);
    findStock();
    NvdcMode2 io = g_hw.stock;
    g_hw.stock_ok = R_SUCCEEDED(nvsIoctl(&g_nv, g_fd, NVDISP_VALIDATE_MODE2, &io));
    F("Mode DB key %08X (%u modes), dock max TMDS %u kHz, stock mode %s\n", g_hw.key, g_hw.db.num_modes,
      g_hw.dock.max_tmds_khz, g_hw.stock_ok ? "ok" : "REJECTED");
    g_hw.ready = true;
}

void hwExit(void) {
    if (g_vm.open) vimagClose(&g_vm);
    if (g_fd_open) nvsDevClose(&g_nv, g_fd);
    g_fd_open = false;
    if (g_nv.active) nvsClose(&g_nv);
    g_hw.ready = false;
}

bool hwLiveMode(NvdcMode2 *m) {
    memset(m, 0, sizeof(*m));
    if (!g_fd_open) return false;
    if (R_FAILED(nvsIoctl(&g_nv, g_fd, NVDISP_GET_MODE2, m))) return false;
    return m->hActive && m->vActive && m->pclkKHz;
}

bool hwIsStock(const NvdcMode2 *m) {
    return nvdcTimingEq(m, &g_hw.stock);
}

Result hwSetMode(const NvdcMode2 *m) {
    if (!g_fd_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    NvdcMode2 io = *m;
    Result rc = nvsIoctl(&g_nv, g_fd, NVDISP_VALIDATE_MODE2, &io);
    if (R_SUCCEEDED(rc)) {
        io = *m;
        rc = nvsIoctl(&g_nv, g_fd, NVDISP_SET_MODE2, &io);
    }
    F("SET_MODE2 %ux%u %u kHz -> rc 0x%x nv 0x%x\n", m->hActive, m->vActive, m->pclkKHz, rc, g_nv.last_nv_error);
    return rc;
}

void hwRestoreStock(void) {
    NvdcMode2 cur;
    if (g_hw.stock_ok && hwLiveMode(&cur) && !is16x9(cur.hActive, cur.vActive)) hwSetMode(&g_hw.stock);
}

bool hwFindSharp(double ar, NvdcMode2 *out) {
    if (!g_hw.ready || ar >= CONTENT_AR * 0.99) return false;
    const NvdcMode2 *best = NULL;
    u32 n = g_hw.db.num_modes > 201 ? 201 : g_hw.db.num_modes;
    for (u32 i = 0; i < n; i++) {
        const NvdcMode2 *m = &g_hw.db.modes[i];
        if (!arMatches(m->hActive, m->vActive, ar) || m->hActive < 1920 || m->vActive < 1080) continue;
        if (g_hw.dock.max_tmds_khz && m->pclkKHz > g_hw.dock.max_tmds_khz) continue;
        if (fabs(nvdcHz(m) - 60.0) > 1.0) continue;
        NvdcMode2 io = *m;
        if (R_FAILED(nvsIoctl(&g_nv, g_fd, NVDISP_VALIDATE_MODE2, &io))) continue;
        if (!best || m->hActive * m->vActive < best->hActive * best->vActive) best = m;
    }
    if (best) { *out = *best; return true; }
    u32 w, h;
    idealNativeMode(ar, &w, &h);
    NvdcMode2 m = cvtRb(w, h, 60);
    if (g_hw.dock.max_tmds_khz && m.pclkKHz > g_hw.dock.max_tmds_khz) return false;
    NvdcMode2 io = m;
    if (R_FAILED(nvsIoctl(&g_nv, g_fd, NVDISP_VALIDATE_MODE2, &io))) return false;
    *out = m;
    return true;
}

// ---------------------------------------------------------------------------

static bool ensureVi(void) {
    if (g_vm.open) return true;
    Result first = 0;
    Result rc = vimagOpen(&g_vm, &first);
    if (R_FAILED(rc)) F("vi:m open failed 0x%x (first 0x%x)\n", rc, first);
    return R_SUCCEEDED(rc);
}

Result hwZoomSet(const s32 ref[4]) {
    if (!ensureVi()) return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    s32 lw = 1920, lh = 1080, z[4];
    vimagGetLogicalResolution(&g_vm, &lw, &lh);
    zoomToLogical(ref, lw, lh, z);
    return vimagSet(&g_vm, z[0], z[1], z[2], z[3]);
}

Result hwZoomReset(void) {
    static const s32 ident[4] = { 0, 0, 1920, 1080 };
    Result rc = hwZoomSet(ident);
    F("zoom reset: rc 0x%x\n", rc);
    return rc;
}

// ---------------------------------------------------------------------------

bool sysmodRunning(void) {
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, SS_TID)) && pid;
}

Result sysmodStart(void) {
    NcmProgramLocation loc = { .program_id = SS_TID, .storageID = NcmStorageId_None };
    u64 pid = 0;
    Result rc = pmshellLaunchProgram(0, &loc, &pid);
    F("start service: rc 0x%x pid %llu\n", rc, (unsigned long long)pid);
    return rc;
}

Result sysmodStop(void) {
    Result rc = pmshellTerminateProgram(SS_TID);
    F("stop service: rc 0x%x\n", rc);
    return rc;
}

bool sysmodInstalled(void) {
    return fileExists(SS_CONTENT_DIR "/exefs.nsp");
}

bool bootFlagGet(void) {
    return fileExists(SS_BOOT_FLAG);
}

bool bootFlagSet(bool on) {
    if (!on) return unlink(SS_BOOT_FLAG) == 0 || !fileExists(SS_BOOT_FLAG);
    mkdir(SS_CONTENT_DIR "/flags", 0777);
    return touchFile(SS_BOOT_FLAG);
}
