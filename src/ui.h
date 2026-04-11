#pragma once

#include <stdbool.h>
#include "db.h"
#include "canvas.h"
#include "sqlite3.h"

typedef enum UIMode {
    UI_NONE = 0,
    UI_SAVE_DIALOG,
    UI_LOAD_LIST,
    UI_CONFIRM_NEW,
    UI_EXPORT_DIALOG,
} UIMode;

typedef struct {
    UIMode        mode;
    // Save dialog
    char          text_input[128];
    int           text_len;
    float         cursor_blink_t;
    // Load list
    PaintingMeta *load_list;
    int           load_count;
    int           load_scroll;
    int           load_selected;
} UIState;

void ui_init(UIState *u);
void ui_free(UIState *u);

// Called each frame when mode != UI_NONE. Returns true when modal is dismissed.
void ui_update(UIState *u, Canvas *canvas, sqlite3 *db);
void ui_draw(const UIState *u);
