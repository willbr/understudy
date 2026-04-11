#include "tools.h"

void tools_init(ToolState *t) {
    t->active_tool = TOOL_BRUSH;
    t->draw_color  = SKYBLUE;
    t->brush_radius = 8;
}

Color tools_get_draw_color(const ToolState *t) {
    if (t->active_tool == TOOL_ERASER) return WHITE;
    return t->draw_color;
}
