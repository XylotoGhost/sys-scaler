#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "log.h"

static FILE *g_log;

bool logOpen(const char *path, size_t max_size) {
    struct stat st;
    bool restart = stat(path, &st) == 0 && (size_t)st.st_size > max_size;
    g_log = fopen(path, restart ? "w" : "a");
    return g_log != NULL;
}

void logClose(void) {
    if (g_log) fclose(g_log);
    g_log = NULL;
}

void F(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log); // keep the log usable even if something later hangs or crashes
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

void logMode2(const char *label, const NvdcMode2 *m) {
    F("  %s: %ux%u @ %.3f Hz, pclk %u kHz, H fp %u sync %u bp %u, V fp %u sync %u bp %u, "
      "vmode 0x%x sync 0x%x bpp %u unk0 0x%x unk1 0x%x res 0x%x\n",
      label, m->hActive, m->vActive, nvdcHz(m), m->pclkKHz, m->hFrontPorch, m->hSyncWidth,
      m->hBackPorch, m->vFrontPorch, m->vSyncWidth, m->vBackPorch, m->vmode, m->sync,
      m->bitsPerPixel, m->unk0, m->unk1, m->reserved);
}

bool writeFile(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

u64 nowMs(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}
