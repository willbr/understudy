#pragma once

#include "tools.h"
#include "raylib.h"

typedef struct Canvas Canvas;   // forward declaration

typedef struct {
    bool wants_new;
    bool wants_save;
    bool wants_load;
} ToolbarEvents;

void toolbar_draw(const ToolState *t, const Canvas *c);
ToolbarEvents toolbar_update(ToolState *t, Canvas *c);
