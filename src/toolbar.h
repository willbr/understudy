#pragma once

#include "tools.h"
#include "raylib.h"

// Forward-declare UIMode so toolbar can request modal changes
// without a circular include; ui.h includes toolbar.h.
typedef enum UIMode UIMode;

typedef struct {
    bool wants_new;
    bool wants_save;
    bool wants_load;
} ToolbarEvents;

void toolbar_draw(const ToolState *t);
ToolbarEvents toolbar_update(ToolState *t);
