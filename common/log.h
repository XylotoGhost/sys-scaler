#pragma once
#include <switch.h>
#include "nvdisp.h"

// append = keep earlier runs in the same file
bool logOpen(const char *path, bool append);
// Sysmodules have no console: L() then only writes to the file.
void logSetConsole(bool on);
void logClose(void);

// Screen + file.
void L(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// File only (verbose detail that would flood the 80x45 console).
void F(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Hex dump, file only.
void logHex(const char *label, const void *data, size_t len);

bool writeFile(const char *path, const void *data, size_t len);

// One line to screen (if screen) and every field to the file.
void logMode2(const char *label, const NvdcMode2 *m, bool screen);
