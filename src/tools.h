#pragma once

#include "raylib.h"

typedef enum {
    TOOL_BRUSH = 0,
    TOOL_ERASER,
    TOOL_LINE
} ToolType;

typedef struct {
    ToolType active_tool;
    Color    draw_color;
    int      brush_radius;  // 1..200
} ToolState;

void tools_init(ToolState *t);
Color tools_get_draw_color(const ToolState *t);
