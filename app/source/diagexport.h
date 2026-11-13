#pragma once
#include <switch.h>

// Writes sdmc:/config/sys-scaler/diag/. Closes the current log; reopen it afterwards.
bool diagExport(void);
