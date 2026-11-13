// Minimal software UI: 1280x720 linear framebuffer, the system's shared font via
// FreeType (incl. the Nintendo button glyphs), and a few primitives.
#pragma once
#include <switch.h>

#define UI_W 1280
#define UI_H 720

// Framebuffer pixel = RGBA8888 in memory order R, G, B, A.
#define RGB(r, g, b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | 0xFF000000u)

// Nintendo extended font (shared font "NintendoExt").
#define G_A     ""
#define G_B     ""
#define G_X     ""
#define G_Y     ""
#define G_L     ""
#define G_R     ""
#define G_ZL    ""
#define G_ZR    ""
#define G_DPAD  ""
#define G_PLUS  ""
#define G_MINUS ""

bool uiInit(void);
void uiExit(void);
void uiBegin(u32 bg);
void uiEnd(void);

void uiFill(int x, int y, int w, int h, u32 c);
void uiBlend(int x, int y, int w, int h, u32 c, u8 alpha);
void uiRoundRect(int x, int y, int w, int h, int r, u32 c);
void uiFrame(int x, int y, int w, int h, int t, u32 c);
void uiRing(int cx, int cy, int r, int t, u32 c);     // anti-aliased circle outline
void uiDisc(int cx, int cy, int r, u32 c);            // anti-aliased filled circle
void uiArrow(int x, int y, int h, bool right, u32 c); // small triangle, (x,y) = top-left

// Text: y is the top of the line box (size px tall). Returns the advance width.
int  uiText(int x, int y, int size, u32 c, const char *utf8);
int  uiTextWidth(int size, const char *utf8);
int  uiTextRight(int right, int y, int size, u32 c, const char *utf8);
int  uiTextCenter(int cx, int y, int size, u32 c, const char *utf8);
// Word-wrapped; returns the height used.
int  uiTextWrap(int x, int y, int w, int size, int line_h, u32 c, const char *utf8);
