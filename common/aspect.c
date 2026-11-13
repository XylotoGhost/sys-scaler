#include <math.h>
#include <stdio.h>
#include "aspect.h"

double panelAspect(const EdidInfo *e, const char *override_path, const char **src) {
    const char *dummy;
    if (!src) src = &dummy;
    u32 a = 0, b = 0;
    FILE *f = override_path ? fopen(override_path, "r") : NULL;
    if (f) {
        bool ok = fscanf(f, "%u:%u", &a, &b) == 2 && a && b;
        fclose(f);
        if (ok) { *src = "override file"; return (double)a / b; }
    }
    if (e && e->native_w && e->native_h) {
        *src = "EDID native resolution";
        return (double)e->native_w / e->native_h;
    }
    if (e && e->screen_w_cm && e->screen_h_cm) {
        *src = "EDID physical size (approximate)";
        return (double)e->screen_w_cm / e->screen_h_cm;
    }
    *src = "default (no EDID)";
    return CONTENT_AR;
}

bool arMatches(u32 w, u32 h, double ar) {
    if (!w || !h || ar <= 0) return false;
    return fabs((double)w / h / ar - 1.0) < 0.01;
}

void arName(double ar, char *buf, size_t n) {
    static const struct { u32 a, b; const char *name; } known[] = {
        {16, 9, "16:9"}, {16, 10, "16:10"}, {4, 3, "4:3"}, {5, 4, "5:4"}, {3, 2, "3:2"},
        {64, 27, "21:9 (64:27)"}, {43, 18, "21:9 (43:18)"}, {12, 5, "21:9 (12:5)"}, {32, 9, "32:9"},
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (fabs(ar / ((double)known[i].a / known[i].b) - 1.0) < 0.01) {
            snprintf(buf, n, "%s", known[i].name);
            return;
        }
    }
    snprintf(buf, n, "%.3f:1", ar);
}

static u32 even(double v) { return (u32)(v / 2.0 + 0.5) * 2; }

Rect fitPrecompensated(double panel_ar, u32 sig_w, u32 sig_h) {
    double sig_ar = (double)sig_w / sig_h;
    // Monitor scales x by Wp/sig_w and y by Hp/sig_h, so a w:h rect shows up as
    // (w/h) * panel_ar / sig_ar. Solve for it showing as 16:9.
    double want = CONTENT_AR * sig_ar / panel_ar;
    Rect r;
    if (want <= sig_ar) { r.h = sig_h; r.w = even(sig_h * want); }  // pillarbox
    else                { r.w = sig_w; r.h = even(sig_w / want); }  // letterbox
    r.x = (sig_w - r.w) / 2;
    r.y = (sig_h - r.h) / 2;
    return r;
}

void zoomRectForPanel(double panel_ar, s32 lw, s32 lh, s32 out[4]) {
    Rect r = fitPrecompensated(panel_ar, (u32)lw, (u32)lh);
    // logical (0,0,lw,lh) must land on r: lw * lw / mw = r.w, -mx * lw / mw = r.x
    double mw = (double)lw * lw / r.w;
    double mh = (double)lh * lh / r.h;
    out[0] = (s32)lround(-(double)r.x * mw / lw);
    out[1] = (s32)lround(-(double)r.y * mh / lh);
    out[2] = (s32)lround(mw);
    out[3] = (s32)lround(mh);
}

void zoomToLogical(const s32 ref[4], s32 lw, s32 lh, s32 out[4]) {
    out[0] = (s32)lround((double)ref[0] * lw / 1920.0);
    out[1] = (s32)lround((double)ref[1] * lh / 1080.0);
    out[2] = (s32)lround((double)ref[2] * lw / 1920.0);
    out[3] = (s32)lround((double)ref[3] * lh / 1080.0);
}

// vi maps the zoom rect (x,y,w,h) onto the whole 1920x1080 display, so the
// logical screen lands at img.x = -x * 1920 / w, img.w = 1920 * 1920 / w.
void zoomFromImage(const Rect *img, s32 out[4]) {
    double mw = 1920.0 * 1920.0 / img->w, mh = 1080.0 * 1080.0 / img->h;
    out[0] = (s32)lround(-(double)img->x * mw / 1920.0);
    out[1] = (s32)lround(-(double)img->y * mh / 1080.0);
    out[2] = (s32)lround(mw);
    out[3] = (s32)lround(mh);
}

Rect zoomToImage(const s32 z[4]) {
    Rect r = { 0, 0, 1920, 1080 };
    if (z[2] <= 0 || z[3] <= 0) return r;
    r.w = (u32)lround(1920.0 * 1920.0 / z[2]);
    r.h = (u32)lround(1080.0 * 1080.0 / z[3]);
    double x = -(double)z[0] * r.w / 1920.0, y = -(double)z[1] * r.h / 1080.0;
    r.x = x > 0 ? (u32)lround(x) : 0;
    r.y = y > 0 ? (u32)lround(y) : 0;
    return r;
}

bool is16x9(u32 w, u32 h) {
    return w && h && w * 9 == h * 16;
}

Rect fitNative(u32 out_w, u32 out_h) {
    double s = fmin(out_w / 1920.0, out_h / 1080.0);
    Rect r = { 0, 0, even(1920 * s), even(1080 * s) };
    r.x = (out_w - r.w) / 2;
    r.y = (out_h - r.h) / 2;
    return r;
}

void idealNativeMode(double panel_ar, u32 *w, u32 *h) {
    if (panel_ar <= CONTENT_AR) { *w = 1920; *h = even(1920 / panel_ar); }
    else                        { *h = 1080; *w = (u32)(1080 * panel_ar / 8.0 + 0.5) * 8; }
}

NvdcMode2 cvtRb(u32 w, u32 h, u32 hz) {
    const double min_vblank_us = 460.0;
    const u32 v_fp = 3, min_v_bp = 6;
    double ar = (double)w / h;
    u32 v_sync = 10;
    if      (fabs(ar / (4.0 / 3) - 1) < 0.01)   v_sync = 4;
    else if (fabs(ar / (16.0 / 9) - 1) < 0.01)  v_sync = 5;
    else if (fabs(ar / (16.0 / 10) - 1) < 0.01) v_sync = 6;
    else if (fabs(ar / (5.0 / 4) - 1) < 0.01 || fabs(ar / (15.0 / 9) - 1) < 0.01) v_sync = 7;

    u32 hact = w / 8 * 8;
    double h_period_us = (1000000.0 / hz - min_vblank_us) / h;
    u32 vbi = (u32)floor(min_vblank_us / h_period_us) + 1;
    if (vbi < v_fp + v_sync + min_v_bp) vbi = v_fp + v_sync + min_v_bp;
    u32 vtotal = h + vbi;
    u32 htotal = hact + 160;
    double mhz = (double)hz * vtotal * htotal / 1e6;
    mhz = floor(mhz / 0.25) * 0.25;

    NvdcMode2 m = {0};
    m.bitsPerPixel = 24;
    m.sync = NVDC_SYNC_HPOS; // CVT-RB: +HSync, -VSync
    m.hActive = hact; m.hFrontPorch = 48; m.hSyncWidth = 32; m.hBackPorch = 80;
    m.vActive = h;    m.vFrontPorch = v_fp; m.vSyncWidth = v_sync; m.vBackPorch = vbi - v_fp - v_sync;
    m.pclkKHz = (u32)(mhz * 1000.0 + 0.5);
    return m;
}
