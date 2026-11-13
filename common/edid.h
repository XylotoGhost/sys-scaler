// The parts of the monitor's EDID that sys-scaler uses. Everything else is left
// to offline tools (the diagnostics export writes the raw EDID).
#pragma once
#include <switch.h>

typedef struct {
    char name[14];                   // monitor name descriptor, "" if none
    u32  native_w, native_h;         // preferred (first) detailed timing, 0 if none
    u32  screen_w_cm, screen_h_cm;   // 0 if unknown
} EdidInfo;

// false (and *out zeroed) if buf doesn't start with an EDID base block.
bool edidParse(const u8 *buf, size_t len, EdidInfo *out);

// Blanks the serial number in the base block (ID field and text descriptor) and
// fixes the checksum, so that logs and exports never contain it.
void edidScrubSerial(u8 *buf, size_t len);
