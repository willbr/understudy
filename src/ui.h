#pragma once

#include <stdbool.h>
#include "db.h"
#include "canvas.h"
#include "sqlite3.h"

typedef enum UIMode {
    UI_NONE = 0,
    UI_SAVE_DIALOG,
    UI_LOAD_LIST,
    UI_EXPORT_DIALOG,
    UI_CROP_MODE,
    UI_RESIZE_DIALOG,
    UI_HELP,
} UIMode;

typedef struct {
    UIMode        mode;
    // Save/export dialog
    char          text_input[128];
    int           text_len;
    float         cursor_blink_t;
    char          last_saved_name[128];  // set after save/load for title update
    // Load list
    PaintingMeta *load_list;
    int           load_count;
    int           load_scroll;
    int           load_selected;
    bool          load_renaming;  // inline rename active
    // Crop mode
    Vector2       crop_start;
    Vector2       crop_end;
    bool          crop_dragging;
    bool          crop_rect_valid;
    // Resize dialog
    char          resize_w_buf[16];
    int           resize_w_len;
    char          resize_h_buf[16];
    int           resize_h_len;
    bool          resize_lock_aspect;
    int           resize_active_field;  // 0=width, 1=height
    float         resize_aspect;
} UIState;

void ui_init(UIState *u);
void ui_free(UIState *u);

void ui_update(UIState *u, Canvas *canvas, sqlite3 *db, int canvas_x);
void ui_draw(const UIState *u, const Canvas *c, int canvas_x);
