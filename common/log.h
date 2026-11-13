#pragma once
#include <switch.h>
#include "nvdisp.h"

// Opens the log for appending. A file already larger than max_size is restarted.
bool logOpen(const char *path, size_t max_size);
void logClose(void);

void F(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logHex(const char *label, const void *data, size_t len);
void logMode2(const char *label, const NvdcMode2 *m);

bool writeFile(const char *path, const void *data, size_t len);

// Milliseconds since boot.
u64 nowMs(void);
