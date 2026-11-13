// Aspect-ratio math shared by the app and the sysmodule.
// Content is always 16:9 (what games render); the panel can be anything.
#pragma once
#include <switch.h>
#include "nvdisp.h"
#include "edid.h"

#define CONTENT_AR (16.0 / 9.0)

// Panel aspect: EDID native resolution > EDID physical size > 16:9.
// *src (optional) receives a short description of the source.
double panelAspect(const EdidInfo *e, const char **src);

typedef struct { u32 x, y, w, h; } Rect;

// Known monitor shapes. Index 0 is "Auto" (w = h = 0); hint is an optional
// addition for the shape picker, e.g. the typical resolution.
typedef struct { u32 w, h; const char *name, *hint; } Shape;
extern const Shape g_shapes[];
extern const int   g_num_shapes;
// Index of the known shape closest to ar (within 1%), 0 if none.
int  shapeMatch(double ar);
// "16:10", "4:3", "21:9", ... or "1.778:1"
void arName(double ar, char *buf, size_t n);
// ar as the fraction stored in a profile: a known shape, else ar * 1000 : 1000.
void arToFraction(double ar, u32 *w, u32 *h);

// true if w:h is within 1% of ar
bool arMatches(u32 w, u32 h, double ar);

// Approach A: the signal stays sig_w x sig_h (16:9) and the monitor stretches it
// to fill a panel of aspect panel_ar. Returns where to put the 16:9 game image
// inside the signal so it looks correct after that stretch (bars top/bottom for
// panels taller than 16:9, left/right for wider ones).
Rect fitPrecompensated(double panel_ar, u32 sig_w, u32 sig_h);

// Approach A done by vi: the display-magnification rect (x, y, w, h in 1920x1080
// reference units, mapped onto the whole display) that draws the 16:9 image
// exactly at fitPrecompensated(panel_ar, 1920, 1080). 16:10 -> (0, -60, 1920, 1200).
// Confirmed on device: vi accepts negative x/y and maps non-uniformly; it clips
// to the display it set.
void zoomRectForPanel(double panel_ar, s32 out[4]);

// Profiles store the zoom rect in 1920x1080 reference units; this scales it to
// the display's actual logical resolution (e.g. 1280x720).
void zoomToLogical(const s32 ref[4], s32 lw, s32 lh, s32 out[4]);
// Image rect on screen <-> zoom rect, both in reference units (fine-tuning).
void zoomFromImage(const Rect *img, s32 out[4]);
Rect zoomToImage(const s32 z[4]);

// 16:9 output mode (1920x1080, 1280x720): the centered fix applies.
bool is16x9(u32 w, u32 h);

// Approach B: the signal itself has the panel's aspect. Smallest output mode
// with the panel's aspect that holds 1920x1080 unscaled
// (16:10 -> 1920x1200, 4:3 -> 1920x1440, 64:27 -> 2560x1080).
void idealNativeMode(double panel_ar, u32 *w, u32 *h);

// VESA CVT reduced-blanking (v1) timing for w x h @ hz, filled in the same shape
// the driver uses for DMT entries in its mode DB (bpp 24, vmode 0, +H -V, no CEA aspect).
NvdcMode2 cvtRb(u32 w, u32 h, u32 hz);
