#pragma once
#include <switch.h>

// Writes sdmc:/config/sys-scaler/diag/ for the connected monitor (needs
// g_hw.ready). Uses the logger: close the current log before, reopen it after.
bool diagExport(void);
