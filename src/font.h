#pragma once

#include "raylib.h"

// Global UI font — loaded in main.c, used by all drawing code
extern Font g_font;

// Helper: DrawTextEx wrapper with simpler API (like DrawText but uses g_font)
static inline void DrawUI(const char *text, int x, int y, int size, Color color) {
    DrawTextEx(g_font, text, (Vector2){(float)x, (float)y},
               (float)size, 1.0f, color);
}

static inline int MeasureUI(const char *text, int size) {
    Vector2 v = MeasureTextEx(g_font, text, (float)size, 1.0f);
    return (int)v.x;
}
