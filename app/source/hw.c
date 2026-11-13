#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "hw.h"
#include "log.h"
#include "settings.h"
#include "vimag.h"

// /dev/nvdisp-* needs nvdrv permission bit 8, which of the services an app can
// reach only nvdrv:s has. libnx picks the service once per process, so the
// framebuffer's nv session runs on nvdrv:s as well.
u32 __nx_nv_service_type = NvServiceType_System;

Hw g_hw;
static bool  g_nv_ok, g_fd_open;
static u32   g_fd;
static ViMag g_vm;

double shapeAr(int idx) {
    if (idx <= 0 || idx >= g_num_shapes) return g_hw.ar_auto;
    return (double)g_shapes[idx].w / g_shapes[idx].h;
}

// ---------------------------------------------------------------------------

static void findStock(void) {
    for (u32 i = 0; i < nvdcDbCount(&g_hw.db); i++) {
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

// Fills g_hw for the connected monitor; false with g_hw.error set if display
// access doesn't work.
static bool openDisplay(void) {
    SetSysEdid e;
    memset(&e, 0, sizeof(e));
    if (R_SUCCEEDED(setsysGetEdid(&e))) {
        memcpy(g_hw.edid_raw, &e, sizeof(e));
        size_t len = (1 + g_hw.edid_raw[126]) * 128;
        if (len > sizeof(g_hw.edid_raw)) len = sizeof(g_hw.edid_raw);
        edidScrubSerial(g_hw.edid_raw, len);
        if (edidParse(g_hw.edid_raw, len, &g_hw.edid)) g_hw.edid_len = len;
    }
    const char *src;
    g_hw.ar_auto = panelAspect(&g_hw.edid, &src);
    arName(g_hw.ar_auto, g_hw.ar_auto_name, sizeof(g_hw.ar_auto_name));

    Result rc = nvInitialize();
    if (R_FAILED(rc)) { snprintf(g_hw.error, sizeof(g_hw.error), "nvdrv:s unavailable (0x%x)", rc); return false; }
    g_nv_ok = true;
    rc = nvOpen(&g_fd, "/dev/nvdisp-disp1");
    if (R_FAILED(rc)) { snprintf(g_hw.error, sizeof(g_hw.error), "Display access failed (0x%x)", rc); return false; }
    g_fd_open = true;
    rc = nvIoctl(g_fd, NVDISP_GET_MODE_DB2, &g_hw.db);
    if (R_FAILED(rc)) {
        snprintf(g_hw.error, sizeof(g_hw.error), "Can't read the monitor's modes (0x%x)", rc);
        return false;
    }
    g_hw.key = profileKey(&g_hw.db);
    dockRead(g_fd, &g_hw.dock);
    findStock();
    NvdcMode2 io = g_hw.stock;
    g_hw.stock_ok = R_SUCCEEDED(nvIoctl(g_fd, NVDISP_VALIDATE_MODE2, &io));
    F("Monitor \"%s\", panel %s from %s, key %08X (%u modes), dock max TMDS %u kHz, stock mode %s\n",
      g_hw.edid.name, g_hw.ar_auto_name, src, g_hw.key, g_hw.db.num_modes, g_hw.dock.max_tmds_khz,
      g_hw.stock_ok ? "ok" : "REJECTED");
    return true;
}

void hwInit(bool docked) {
    static char logged_error[sizeof(g_hw.error)];   // hwStale retries every second: log a failure once
    hwExit();
    memset(&g_hw, 0, sizeof(g_hw));
    g_hw.ar_auto = CONTENT_AR;
    snprintf(g_hw.ar_auto_name, sizeof(g_hw.ar_auto_name), "16:9");
    if (!docked) {
        snprintf(g_hw.error, sizeof(g_hw.error), "Not docked");
        return;
    }
    g_hw.ready = openDisplay();
    if (!g_hw.ready && strcmp(g_hw.error, logged_error) != 0) F("Display not ready: %s\n", g_hw.error);
    snprintf(logged_error, sizeof(logged_error), "%s", g_hw.error);
}

void hwExit(void) {
    if (g_vm.open) vimagClose(&g_vm);
    if (g_fd_open) nvClose(g_fd);
    g_fd_open = false;
    if (g_nv_ok) nvExit();
    g_nv_ok = false;
    g_hw.ready = false;
}

bool hwStale(void) {
    static NvdcModeDB2 db;   // 12 KB, keep off the stack
    if (!g_hw.ready) return true;
    return R_FAILED(nvIoctl(g_fd, NVDISP_GET_MODE_DB2, &db)) || profileKey(&db) != g_hw.key;
}

bool hwLiveMode(NvdcMode2 *m) {
    memset(m, 0, sizeof(*m));
    if (!g_fd_open) return false;
    if (R_FAILED(nvIoctl(g_fd, NVDISP_GET_MODE2, m))) return false;
    return m->hActive && m->vActive && m->pclkKHz;
}

Result hwSetMode(const NvdcMode2 *m) {
    if (!g_fd_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    NvdcMode2 io = *m;
    Result rc = nvIoctl(g_fd, NVDISP_VALIDATE_MODE2, &io);
    if (R_SUCCEEDED(rc)) {
        io = *m;
        rc = nvIoctl(g_fd, NVDISP_SET_MODE2, &io);
    }
    F("SET_MODE2 %ux%u %u kHz -> rc 0x%x\n", m->hActive, m->vActive, m->pclkKHz, rc);
    return rc;
}

void hwRestoreStock(void) {
    NvdcMode2 cur;
    if (g_hw.stock_ok && hwLiveMode(&cur) && !is16x9(cur.hActive, cur.vActive)) hwSetMode(&g_hw.stock);
}

bool hwFindSharp(double ar, NvdcMode2 *out) {
    if (!g_hw.ready || ar >= CONTENT_AR * 0.99) return false;
    const NvdcMode2 *best = NULL;
    for (u32 i = 0; i < nvdcDbCount(&g_hw.db); i++) {
        const NvdcMode2 *m = &g_hw.db.modes[i];
        if (!arMatches(m->hActive, m->vActive, ar) || m->hActive < 1920 || m->vActive < 1080) continue;
        if (g_hw.dock.max_tmds_khz && m->pclkKHz > g_hw.dock.max_tmds_khz) continue;
        if (fabs(nvdcHz(m) - 60.0) > 1.0) continue;
        NvdcMode2 io = *m;
        if (R_FAILED(nvIoctl(g_fd, NVDISP_VALIDATE_MODE2, &io))) continue;
        if (!best || m->hActive * m->vActive < best->hActive * best->vActive) best = m;
    }
    if (best) { *out = *best; return true; }
    u32 w, h;
    idealNativeMode(ar, &w, &h);
    NvdcMode2 m = cvtRb(w, h, 60);
    if (g_hw.dock.max_tmds_khz && m.pclkKHz > g_hw.dock.max_tmds_khz) return false;
    NvdcMode2 io = m;
    if (R_FAILED(nvIoctl(g_fd, NVDISP_VALIDATE_MODE2, &io))) return false;
    *out = m;
    return true;
}

// ---------------------------------------------------------------------------

Result hwZoomSet(const s32 ref[4]) {
    return vimagSetRef(&g_vm, ref, NULL);
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
