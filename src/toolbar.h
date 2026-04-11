#pragma once

#include "tools.h"
#include "raylib.h"

typedef struct Canvas Canvas;   // forward declaration

typedef struct {
    bool wants_new;
    bool wants_save;
    bool wants_load;
    bool wants_export;
    bool wants_crop;
    bool wants_resize;
} ToolbarEvents;

void toolbar_draw(const ToolState *t, const Canvas *c);
ToolbarEvents toolbar_update(ToolState *t, Canvas *c);
