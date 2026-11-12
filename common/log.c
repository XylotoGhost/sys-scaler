#include <stdio.h>
#include <stdarg.h>
#include "log.h"

static FILE *g_log;
static bool  g_console = true;

void logSetConsole(bool on) { g_console = on; }

bool logOpen(const char *path, bool append) {
    g_log = fopen(path, append ? "a" : "w");
    return g_log != NULL;
}

void logClose(void) {
    if (g_log) fclose(g_log);
    g_log = NULL;
}

static void vfile(const char *fmt, va_list ap) {
    if (!g_log) return;
    vfprintf(g_log, fmt, ap);
    fflush(g_log); // keep the log usable even if something later hangs or crashes
}

void L(const char *fmt, ...) {
    va_list ap;
    if (g_console) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    va_start(ap, fmt);
    vfile(fmt, ap);
    va_end(ap);
    if (g_console) consoleUpdate(NULL);
}

void F(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfile(fmt, ap);
    va_end(ap);
}

void logHex(const char *label, const void *data, size_t len) {
    const u8 *p = data;
    F("%s (%zu bytes):\n", label, len);
    for (size_t i = 0; i < len; i += 16) {
        F("  %04zx:", i);
        for (size_t j = i; j < i + 16 && j < len; j++) F(" %02x", p[j]);
        F("\n");
    }
}

void logMode2(const char *label, const NvdcMode2 *m, bool screen) {
    u32 ht = m->hActive + m->hFrontPorch + m->hSyncWidth + m->hBackPorch;
    u32 vt = m->vActive + m->vFrontPorch + m->vSyncWidth + m->vBackPorch;
    if (screen)
        L("  %s: %ux%u @ %.3f Hz, pclk %u kHz, vmode 0x%x sync %u\n", label,
          m->hActive, m->vActive, nvdcHz(m), m->pclkKHz, m->vmode, m->sync);
    F("    %s fields: unk0=0x%x hAct=%u vAct=%u hSync=%u vSync=%u hFP=%u vFP=%u hBP=%u vBP=%u "
      "pclkKHz=%u bpp=%u vmode=0x%x sync=0x%x unk1=0x%x res=0x%x | htotal=%u vtotal=%u\n",
      label, m->unk0, m->hActive, m->vActive, m->hSyncWidth, m->vSyncWidth, m->hFrontPorch,
      m->vFrontPorch, m->hBackPorch, m->vBackPorch, m->pclkKHz, m->bitsPerPixel, m->vmode,
      m->sync, m->unk1, m->reserved, ht, vt);
}

bool writeFile(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}
