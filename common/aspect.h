// Aspect-ratio math shared by the diagnostic and (later) the real fix.
// Content is always 16:9 (what games render); the panel can be anything.
#pragma once
#include <switch.h>
#include "nvdisp.h"
#include "edid.h"

#define CONTENT_AR (16.0 / 9.0)

// Panel aspect: "W:H" in override_path > EDID native resolution > EDID physical
// size > 16:9. *src (optional) receives a short description of the source.
double panelAspect(const EdidInfo *e, const char *override_path, const char **src);

typedef struct { u32 x, y, w, h; } Rect;

// true if w:h is within 1% of ar
bool arMatches(u32 w, u32 h, double ar);
// "16:10", "4:3", "21:9 (64:27)", ... or "1.778:1"
void arName(double ar, char *buf, size_t n);

// Approach A: the signal stays sig_w x sig_h (16:9) and the monitor stretches it
// to fill a panel of aspect panel_ar. Returns where to put the 16:9 game image
// inside the signal so it looks correct after that stretch (bars top/bottom for
// panels taller than 16:9, left/right for wider ones).
Rect fitPrecompensated(double panel_ar, u32 sig_w, u32 sig_h);

// Approach A done by vi: the display-magnification rect (x, y, w, h in logical
// coordinates, mapped onto the whole lw x lh display) that draws the 16:9 image
// exactly at fitPrecompensated(panel_ar, lw, lh). 16:10 at 1920x1080 ->
// (0, -60, 1920, 1200). Confirmed on device: vi accepts negative x/y and maps
// non-uniformly; it clips to the display it set (run 5).
void zoomRectForPanel(double panel_ar, s32 lw, s32 lh, s32 out[4]);

// Approach B: the signal itself has the panel's aspect (out_w x out_h). Returns
// the largest centered 16:9 rect (1:1 with a 1920x1080 game when it fits).
Rect fitNative(u32 out_w, u32 out_h);

// Smallest output mode with the panel's aspect that holds 1920x1080 unscaled
// (16:10 -> 1920x1200, 4:3 -> 1920x1440, 64:27 -> 2560x1080).
void idealNativeMode(double panel_ar, u32 *w, u32 *h);

// VESA CVT reduced-blanking (v1) timing for w x h @ hz, filled in the same shape
// the driver uses for DMT entries in its mode DB (bpp 24, vmode 0, +H -V, no CEA aspect).
NvdcMode2 cvtRb(u32 w, u32 h, u32 hz);
