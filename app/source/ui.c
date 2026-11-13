#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "ui.h"

static Framebuffer g_fb;
static u32        *g_px;
static u32         g_stride;   // in pixels
static FT_Library  g_ft;
static FT_Face     g_face_std, g_face_ext;
static bool        g_pl, g_fb_ok, g_font_ok;

// ---------------------------------------------------------------------------
// glyph cache

typedef struct {
    u32  cp;
    u16  size;
    bool used;
    s16  left, top, adv;
    u16  w, h;
    u8  *bmp;
} Glyph;

#define CACHE_SIZE 2048
static Glyph g_cache[CACHE_SIZE];

static const Glyph *glyphGet(u32 cp, int size) {
    u32 h = (cp * 2654435761u) ^ ((u32)size * 40503u);
    for (int i = 0; i < CACHE_SIZE; i++) {
        Glyph *g = &g_cache[(h + i) % CACHE_SIZE];
        if (g->used && g->cp == cp && g->size == size) return g;
        if (g->used) continue;

        FT_Face face = g_face_std;
        FT_UInt idx = FT_Get_Char_Index(g_face_std, cp);
        if (!idx && g_face_ext) {
            idx = FT_Get_Char_Index(g_face_ext, cp);
            face = g_face_ext;
        }
        FT_Set_Pixel_Sizes(face, 0, size);
        if (FT_Load_Glyph(face, idx, FT_LOAD_RENDER)) return NULL;
        FT_GlyphSlot s = face->glyph;
        g->used = true;
        g->cp = cp;
        g->size = size;
        g->left = s->bitmap_left;
        g->top = s->bitmap_top;
        g->adv = s->advance.x >> 6;
        g->w = s->bitmap.width;
        g->h = s->bitmap.rows;
        g->bmp = NULL;
        if (g->w && g->h) {
            g->bmp = malloc(g->w * g->h);
            if (g->bmp)
                for (int y = 0; y < g->h; y++)
                    memcpy(g->bmp + y * g->w, s->bitmap.buffer + y * s->bitmap.pitch, g->w);
        }
        return g;
    }
    return NULL;   // cache full
}

static u32 nextCp(const char **p) {
    const u8 *s = (const u8 *)*p;
    u32 c = s[0];
    int n = 1;
    if (c >= 0xF0 && s[1] && s[2] && s[3]) { c = ((c & 7) << 18) | ((s[1] & 63) << 12) | ((s[2] & 63) << 6) | (s[3] & 63); n = 4; }
    else if (c >= 0xE0 && s[1] && s[2])    { c = ((c & 15) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); n = 3; }
    else if (c >= 0xC0 && s[1])            { c = ((c & 31) << 6) | (s[1] & 63); n = 2; }
    *p += n;
    return c;
}

// ---------------------------------------------------------------------------

bool uiInit(void) {
    if (R_FAILED(framebufferCreate(&g_fb, nwindowGetDefault(), UI_W, UI_H, PIXEL_FORMAT_RGBA_8888, 2)))
        return false;
    framebufferMakeLinear(&g_fb);
    g_fb_ok = true;

    if (R_FAILED(plInitialize(PlServiceType_User))) return false;
    g_pl = true;
    PlFontData std, ext;
    if (R_FAILED(plGetSharedFontByType(&std, PlSharedFontType_Standard))) return false;
    if (FT_Init_FreeType(&g_ft)) return false;
    if (FT_New_Memory_Face(g_ft, std.address, std.size, 0, &g_face_std)) return false;
    if (R_SUCCEEDED(plGetSharedFontByType(&ext, PlSharedFontType_NintendoExt)))
        if (FT_New_Memory_Face(g_ft, ext.address, ext.size, 0, &g_face_ext)) g_face_ext = NULL;
    g_font_ok = true;
    return true;
}

void uiExit(void) {
    for (int i = 0; i < CACHE_SIZE; i++) free(g_cache[i].bmp);
    if (g_face_ext) FT_Done_Face(g_face_ext);
    if (g_face_std) FT_Done_Face(g_face_std);
    if (g_ft) FT_Done_FreeType(g_ft);
    if (g_pl) plExit();
    if (g_fb_ok) framebufferClose(&g_fb);
}

void uiBegin(u32 bg) {
    g_px = framebufferBegin(&g_fb, &g_stride);
    g_stride /= 4;
    for (int y = 0; y < UI_H; y++) {
        u32 *row = g_px + y * g_stride;
        for (int x = 0; x < UI_W; x++) row[x] = bg;
    }
}

void uiEnd(void) {
    framebufferEnd(&g_fb);
}

static inline u32 mix(u32 dst, u32 src, u32 a) {
    if (a >= 255) return src;
    u32 ia = 255 - a;
    u32 r = ((src & 0xFF) * a + (dst & 0xFF) * ia) / 255;
    u32 g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia) / 255;
    u32 b = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static inline void plot(int x, int y, u32 c, u32 a) {
    if ((unsigned)x >= UI_W || (unsigned)y >= UI_H || !a) return;
    u32 *p = g_px + y * g_stride + x;
    *p = mix(*p, c, a);
}

void uiFill(int x, int y, int w, int h, u32 c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > UI_W ? UI_W : x + w, y1 = y + h > UI_H ? UI_H : y + h;
    for (int yy = y0; yy < y1; yy++) {
        u32 *row = g_px + yy * g_stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

void uiBlend(int x, int y, int w, int h, u32 c, u8 alpha) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) plot(xx, yy, c, alpha);
}

void uiRoundRect(int x, int y, int w, int h, int r, u32 c) {
    if (r * 2 > h) r = h / 2;
    for (int yy = 0; yy < h; yy++) {
        int inset = 0;
        double d = -1;
        if (yy < r) d = r - yy - 0.5;
        else if (yy >= h - r) d = yy - (h - r) + 0.5;
        if (d >= 0) inset = r - (int)floor(sqrt((double)r * r - d * d));
        uiFill(x + inset, y + yy, w - 2 * inset, 1, c);
    }
}

void uiFrame(int x, int y, int w, int h, int t, u32 c) {
    uiFill(x, y, w, t, c);
    uiFill(x, y + h - t, w, t, c);
    uiFill(x, y, t, h, c);
    uiFill(x + w - t, y, t, h, c);
}

// Coverage of a band |d - r| <= t/2, 1 px soft edge.
void uiRing(int cx, int cy, int r, int t, u32 c) {
    double ro = r + t / 2.0 + 1, ri = r - t / 2.0 - 1;
    for (int dy = -(int)ro; dy <= (int)ro; dy++) {
        double xo = sqrt(fmax(0, ro * ro - dy * dy));
        double xi = ri > fabs((double)dy) ? sqrt(ri * ri - dy * dy) : 0;
        for (int dx = (int)xi; dx <= (int)xo + 1; dx++) {
            double d = sqrt((double)dx * dx + (double)dy * dy);
            double cov = t / 2.0 + 0.5 - fabs(d - r);
            if (cov <= 0) continue;
            u32 a = cov >= 1 ? 255 : (u32)(cov * 255);
            plot(cx + dx, cy + dy, c, a);
            if (dx) plot(cx - dx, cy + dy, c, a);
        }
    }
}

void uiDisc(int cx, int cy, int r, u32 c) {
    for (int dy = -r - 1; dy <= r + 1; dy++)
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            double cov = r + 0.5 - sqrt((double)dx * dx + (double)dy * dy);
            if (cov > 0) plot(cx + dx, cy + dy, c, cov >= 1 ? 255 : (u32)(cov * 255));
        }
}

void uiArrow(int x, int y, int h, bool right, u32 c) {
    int half = h / 2;
    for (int yy = 0; yy < h; yy++) {
        int len = (yy <= half ? yy : h - 1 - yy) + 1;
        if (right) uiFill(x, y + yy, len, 1, c);
        else uiFill(x + half - len + 1, y + yy, len, 1, c);
    }
}

// ---------------------------------------------------------------------------
// text

static int ascender(int size) {
    FT_Set_Pixel_Sizes(g_face_std, 0, size);
    return g_face_std->size->metrics.ascender >> 6;
}

int uiText(int x, int y, int size, u32 c, const char *s) {
    if (!g_font_ok || !s) return 0;
    int base = y + ascender(size) - size / 12;
    int pen = x;
    while (*s) {
        u32 cp = nextCp(&s);
        const Glyph *g = glyphGet(cp, size);
        if (!g) continue;
        if (g->bmp) {
            int gx = pen + g->left, gy = base - g->top;
            for (int yy = 0; yy < g->h; yy++)
                for (int xx = 0; xx < g->w; xx++) {
                    u8 a = g->bmp[yy * g->w + xx];
                    if (a) plot(gx + xx, gy + yy, c, a);
                }
        }
        pen += g->adv;
    }
    return pen - x;
}

int uiTextWidth(int size, const char *s) {
    if (!g_font_ok || !s) return 0;
    int w = 0;
    while (*s) {
        const Glyph *g = glyphGet(nextCp(&s), size);
        if (g) w += g->adv;
    }
    return w;
}

int uiTextRight(int right, int y, int size, u32 c, const char *s) {
    int w = uiTextWidth(size, s);
    uiText(right - w, y, size, c, s);
    return w;
}

int uiTextCenter(int cx, int y, int size, u32 c, const char *s) {
    int w = uiTextWidth(size, s);
    uiText(cx - w / 2, y, size, c, s);
    return w;
}

int uiTextWrap(int x, int y, int w, int size, int line_h, u32 c, const char *s) {
    char line[256];
    int used = 0;
    const char *p = s;
    while (*p) {
        // take words until the line is full or a newline
        int len = 0, last_space = -1;
        while (p[len] && p[len] != '\n' && len < (int)sizeof(line) - 1) {
            if (p[len] == ' ') last_space = len;
            memcpy(line, p, len + 1);
            line[len + 1] = 0;
            if (uiTextWidth(size, line) > w && last_space > 0) {
                len = last_space;
                break;
            }
            len++;
        }
        memcpy(line, p, len);
        line[len] = 0;
        uiText(x, y + used, size, c, line);
        used += line_h;
        p += len;
        if (*p == ' ' || *p == '\n') p++;
    }
    return used;
}
