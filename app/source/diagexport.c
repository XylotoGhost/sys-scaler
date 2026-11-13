// Diagnostics export (Expert): what a bug report needs and the SD card doesn't
// hold yet. Writes diag.txt, edid.bin (serial number blanked) and modedb2.bin to
// sdmc:/config/sys-scaler/diag/; the logs, config.ini and profiles sit next to
// that folder. Read-only: it reports what hwInit already collected.

#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>

#include "diagexport.h"
#include "hw.h"
#include "log.h"
#include "settings.h"

bool diagExport(void) {
    mkdir(SS_DIAG_DIR, 0777);
    if (!logOpen(SS_DIAG_DIR "/diag.txt", 0)) return false;
    F(SS_NAME " " SS_VERSION " diagnostics\n");

    u32 hv = hosversionGet();
    F("HOS %u.%u.%u", HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv));
    u64 exo = 0;
    if (R_SUCCEEDED(splInitialize())) {
        if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &exo)))
            F(", Atmosphere %u.%u.%u", (u32)(exo >> 56) & 0xFF, (u32)(exo >> 48) & 0xFF, (u32)(exo >> 40) & 0xFF);
        splExit();
    }
    SetSysProductModel model = SetSysProductModel_Invalid;
    setsysGetProductModel(&model);
    F(", product model %d\n", model);
    SetSysTvSettings tv = {0};
    if (R_SUCCEEDED(setsysGetTvSettings(&tv)))
        F("TV settings: resolution %d, underscan %u, rgb_range %d, content_type %d, flags 0x%x\n",
          tv.tv_resolution, tv.underscan, tv.rgb_range, tv.hdmi_content_type, tv.flags);

    F("Monitor \"%s\": native %ux%u, %ux%u cm, panel %s, key %08X\n", g_hw.edid.name, g_hw.edid.native_w,
      g_hw.edid.native_h, g_hw.edid.screen_w_cm, g_hw.edid.screen_h_cm, g_hw.ar_auto_name, g_hw.key);
    Profile p;
    F("Profile: %s\n", profileLoad(g_hw.key, &p) ? profileMethodName(p.method) : "none");
    NvdcMode2 m;
    if (hwLiveMode(&m)) logMode2("live", &m);
    if (hwFindSharp(g_hw.ar_auto, &m)) logMode2("sharp", &m);
    else F("  sharp: not available\n");
    dockLog(&g_hw.dock);
    F("Mode DB, %u modes:\n", g_hw.db.num_modes);
    for (u32 i = 0; i < nvdcDbCount(&g_hw.db); i++) {
        char label[16];
        snprintf(label, sizeof(label), "DB[%u]", i);
        logMode2(label, &g_hw.db.modes[i]);
    }
    logClose();

    bool ok = writeFile(SS_DIAG_DIR "/modedb2.bin", &g_hw.db, sizeof(g_hw.db));
    if (g_hw.edid_len) ok &= writeFile(SS_DIAG_DIR "/edid.bin", g_hw.edid_raw, g_hw.edid_len);
    return ok;
}
