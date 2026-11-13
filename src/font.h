#pragma once

#include "raylib.h"

// Global UI font — loaded in main.c, used by all drawing code
extern Font g_font;

// All UI text is drawn this much larger than the nominal size passed by the
// caller. Layout code keeps its nominal sizes; y is nudged up so the larger
// glyphs stay vertically centred where the nominal ones were.
#define UI_FONT_SCALE 1.25f

// Helper: DrawTextEx wrapper with simpler API (like DrawText but uses g_font)
static inline void DrawUI(const char *text, int x, int y, int size, Color color) {
    float scaled = (float)size * UI_FONT_SCALE;
    float dy     = (scaled - (float)size) * 0.5f;
    DrawTextEx(g_font, text, (Vector2){(float)x, (float)y - dy},
               scaled, 1.0f, color);
}

static inline int MeasureUI(const char *text, int size) {
    Vector2 v = MeasureTextEx(g_font, text, (float)size * UI_FONT_SCALE, 1.0f);
    return (int)(v.x + 0.5f);
}
