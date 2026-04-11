#include "ui.h"
#include "font.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define WIN_W  GetScreenWidth()
#define WIN_H  GetScreenHeight()

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool ui_button(Rectangle r, const char *label) {
    bool hovered = CheckCollisionPointRec(GetMousePosition(), r);
    Color bg = hovered ? (Color){80, 120, 200, 255} : (Color){60, 60, 60, 255};
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, (Color){140, 140, 140, 255});
    int fw = MeasureUI(label, 14);
    DrawUI(label, (int)(r.x + r.width / 2 - fw / 2),
             (int)(r.y + r.height / 2 - 7), 14, WHITE);
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// ── Public API ────────────────────────────────────────────────────────────────

void ui_init(UIState *u) {
    memset(u, 0, sizeof(*u));
    u->load_selected = -1;
}

void ui_free(UIState *u) {
    if (u->load_list) {
        db_free_list(u->load_list);
        u->load_list  = NULL;
        u->load_count = 0;
    }
}

// ── Save Dialog ───────────────────────────────────────────────────────────────

static void save_dialog_update(UIState *u, Canvas *canvas, sqlite3 *db) {
    // Accumulate typed characters
    int ch;
    while ((ch = GetCharPressed()) != 0) {
        if (u->text_len < 127) {
            u->text_input[u->text_len++] = (char)ch;
            u->text_input[u->text_len]   = '\0';
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) && u->text_len > 0) {
        u->text_input[--u->text_len] = '\0';
    }

    float px = WIN_W / 2.0f - 200;
    float py = WIN_H / 2.0f - 70;
    Rectangle r_ok     = {px + 250, py + 100, 80, 30};
    Rectangle r_cancel = {px + 50,  py + 100, 80, 30};

    if (IsKeyPressed(KEY_ENTER) || ui_button(r_ok, "Save")) {
        if (u->text_len > 0) {
            db_save_painting(db, u->text_input,
                             canvas->layers, canvas->layer_count,
                             canvas->width, canvas->height);
            canvas->dirty = false;
            u->mode = UI_NONE;
        }
    }
    if (IsKeyPressed(KEY_ESCAPE) || ui_button(r_cancel, "Cancel")) {
        u->mode = UI_NONE;
    }
}

static void save_dialog_draw(const UIState *u) {
    float px = WIN_W / 2.0f - 200;
    float py = WIN_H / 2.0f - 70;
    DrawRectangle(0, 0, WIN_W, WIN_H, Fade(BLACK, 0.55f));
    DrawRectangle((int)px, (int)py, 400, 150, (Color){30, 30, 30, 255});
    DrawRectangleLinesEx((Rectangle){px, py, 400, 150}, 1, GRAY);

    DrawUI("Save Painting", (int)px + 10, (int)py + 10, 16, RAYWHITE);

    // Text field
    Rectangle field = {px + 10, py + 45, 380, 30};
    DrawRectangleRec(field, (Color){20, 20, 20, 255});
    DrawRectangleLinesEx(field, 1, (Color){140, 140, 140, 255});
    DrawUI(u->text_input, (int)field.x + 5, (int)field.y + 7, 14, WHITE);

    // Blinking cursor
    if ((int)(u->cursor_blink_t * 2) % 2 == 0) {
        int tx = MeasureUI(u->text_input, 14);
        DrawRectangle((int)field.x + 5 + tx, (int)field.y + 5, 2, 20, WHITE);
    }

    DrawUI(u->text_len == 0 ? "Enter a name..." : "",
             (int)field.x + 6, (int)field.y + 8, 14, (Color){80, 80, 80, 255});

    Rectangle r_ok     = {px + 250, py + 100, 80, 30};
    Rectangle r_cancel = {px + 50,  py + 100, 80, 30};
    ui_button(r_ok, "Save");
    ui_button(r_cancel, "Cancel");
}

// ── Load List ─────────────────────────────────────────────────────────────────

#define LOAD_ROW_H    38
#define LOAD_VISIBLE  10

static void load_list_update(UIState *u, Canvas *canvas, sqlite3 *db) {
    // Scroll with mouse wheel
    int wheel = (int)GetMouseWheelMove();
    u->load_scroll -= wheel;
    int max_scroll = u->load_count - LOAD_VISIBLE;
    if (u->load_scroll < 0)          u->load_scroll = 0;
    if (u->load_scroll > max_scroll)  u->load_scroll = (max_scroll > 0 ? max_scroll : 0);

    float px = WIN_W / 2.0f - 260;
    float py = WIN_H / 2.0f - 220;
    float list_y = py + 40;

    // Row clicks
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        for (int i = 0; i < LOAD_VISIBLE && (i + u->load_scroll) < u->load_count; i++) {
            Rectangle row = {px + 10, list_y + i * LOAD_ROW_H, 500, LOAD_ROW_H - 2};
            if (CheckCollisionPointRec(GetMousePosition(), row)) {
                u->load_selected = i + u->load_scroll;
            }
        }
    }

    Rectangle r_open   = {px + 390, py + 420, 80, 30};
    Rectangle r_cancel = {px + 280, py + 420, 80, 30};
    Rectangle r_delete = {px + 10,  py + 420, 80, 30};

    if (ui_button(r_open, "Open") && u->load_selected >= 0 &&
        u->load_selected < u->load_count) {
        PaintingMeta *m = &u->load_list[u->load_selected];
        Layer *layers = NULL;
        int lcount = 0, w = 0, h = 0;
        if (db_load_painting(db, m->id, &layers, &lcount, &w, &h)) {
            canvas_load_layers(canvas, layers, lcount);
            // canvas_load_layers takes ownership; no free needed here
        }
        ui_free(u);
        u->mode = UI_NONE;
    }

    if (IsKeyPressed(KEY_ESCAPE) || ui_button(r_cancel, "Cancel")) {
        ui_free(u);
        u->mode = UI_NONE;
    }

    if (ui_button(r_delete, "Delete") && u->load_selected >= 0 &&
        u->load_selected < u->load_count) {
        db_delete_painting(db, u->load_list[u->load_selected].id);
        // Refresh list
        db_free_list(u->load_list);
        u->load_list = NULL;
        db_list_paintings(db, &u->load_list, &u->load_count);
        if (u->load_selected >= u->load_count)
            u->load_selected = u->load_count - 1;
    }
}

static void load_list_draw(const UIState *u) {
    float px = WIN_W / 2.0f - 260;
    float py = WIN_H / 2.0f - 220;
    float list_y = py + 40;

    DrawRectangle(0, 0, WIN_W, WIN_H, Fade(BLACK, 0.55f));
    DrawRectangle((int)px, (int)py, 520, 470, (Color){30, 30, 30, 255});
    DrawRectangleLinesEx((Rectangle){px, py, 520, 470}, 1, GRAY);
    DrawUI("Load Painting", (int)px + 10, (int)py + 10, 16, RAYWHITE);

    if (u->load_count == 0) {
        DrawUI("No saved paintings.", (int)px + 10, (int)list_y + 10, 14, LIGHTGRAY);
    }

    for (int i = 0; i < LOAD_VISIBLE && (i + u->load_scroll) < u->load_count; i++) {
        int idx = i + u->load_scroll;
        PaintingMeta *m = &u->load_list[idx];
        Rectangle row = {px + 10, list_y + i * LOAD_ROW_H, 500, LOAD_ROW_H - 2};

        Color bg = (idx == u->load_selected)
                   ? (Color){50, 80, 130, 255}
                   : (Color){45, 45, 45, 255};
        DrawRectangleRec(row, bg);
        DrawRectangleLinesEx(row, 1, (Color){70, 70, 70, 255});

        char info[160];
        snprintf(info, sizeof(info), "%s  (%d strokes)  %s", m->name, m->stroke_count, m->updated_at);
        DrawUI(info, (int)row.x + 6, (int)row.y + 10, 13, WHITE);
    }

    // Scroll indicator
    if (u->load_count > LOAD_VISIBLE) {
        char sc[32];
        snprintf(sc, sizeof(sc), "%d/%d", u->load_scroll + 1, u->load_count);
        DrawUI(sc, (int)px + 10, (int)(list_y + LOAD_VISIBLE * LOAD_ROW_H + 4), 12, GRAY);
    }

    Rectangle r_open   = {px + 390, py + 420, 80, 30};
    Rectangle r_cancel = {px + 280, py + 420, 80, 30};
    Rectangle r_delete = {px + 10,  py + 420, 80, 30};
    ui_button(r_open, "Open");
    ui_button(r_cancel, "Cancel");
    ui_button(r_delete, "Delete");
}

// ── Confirm New ───────────────────────────────────────────────────────────────

static void confirm_new_update(UIState *u, Canvas *canvas) {
    float px = WIN_W / 2.0f - 160;
    float py = WIN_H / 2.0f - 60;
    Rectangle r_yes = {px + 220, py + 80, 80, 30};
    Rectangle r_no  = {px + 50,  py + 80, 80, 30};

    if (ui_button(r_yes, "Yes")) {
        canvas_clear(canvas);
        canvas->dirty = false;
        u->mode = UI_NONE;
    }
    if (IsKeyPressed(KEY_ESCAPE) || ui_button(r_no, "Cancel")) {
        u->mode = UI_NONE;
    }
}

static void confirm_new_draw(void) {
    float px = WIN_W / 2.0f - 160;
    float py = WIN_H / 2.0f - 60;
    DrawRectangle(0, 0, WIN_W, WIN_H, Fade(BLACK, 0.55f));
    DrawRectangle((int)px, (int)py, 320, 130, (Color){30, 30, 30, 255});
    DrawRectangleLinesEx((Rectangle){px, py, 320, 130}, 1, GRAY);
    DrawUI("Discard unsaved changes?", (int)px + 10, (int)py + 15, 15, RAYWHITE);
    DrawUI("Current painting will be lost.", (int)px + 10, (int)py + 40, 13, LIGHTGRAY);

    float bpx = WIN_W / 2.0f - 160;
    float bpy = WIN_H / 2.0f - 60;
    Rectangle r_yes = {bpx + 220, bpy + 80, 80, 30};
    Rectangle r_no  = {bpx + 50,  bpy + 80, 80, 30};
    ui_button(r_yes, "Yes");
    ui_button(r_no,  "Cancel");
}

// ── Export Dialog ─────────────────────────────────────────────────────────────

static void export_dialog_update(UIState *u, Canvas *canvas) {
    int ch;
    while ((ch = GetCharPressed()) != 0) {
        if (u->text_len < 127) {
            u->text_input[u->text_len++] = (char)ch;
            u->text_input[u->text_len]   = '\0';
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) && u->text_len > 0) {
        u->text_input[--u->text_len] = '\0';
    }

    float px = WIN_W / 2.0f - 200;
    float py = WIN_H / 2.0f - 70;
    Rectangle r_ok     = {px + 250, py + 100, 80, 30};
    Rectangle r_cancel = {px + 50,  py + 100, 80, 30};

    if (IsKeyPressed(KEY_ENTER) || ui_button(r_ok, "Export")) {
        if (u->text_len > 0) {
            // Append .png if not present
            char path[256];
            const char *home = getenv("HOME");
            if (strstr(u->text_input, "/") != NULL) {
                snprintf(path, sizeof(path), "%s", u->text_input);
            } else {
                snprintf(path, sizeof(path), "%s/Desktop/%s", home ? home : ".", u->text_input);
            }
            int len = (int)strlen(path);
            if (len < 4 || strcmp(path + len - 4, ".png") != 0) {
                strncat(path, ".png", sizeof(path) - len - 1);
            }
            canvas_export_png(canvas, path);
            u->mode = UI_NONE;
        }
    }
    if (IsKeyPressed(KEY_ESCAPE) || ui_button(r_cancel, "Cancel")) {
        u->mode = UI_NONE;
    }
}

static void export_dialog_draw(const UIState *u) {
    float px = WIN_W / 2.0f - 200;
    float py = WIN_H / 2.0f - 70;
    DrawRectangle(0, 0, WIN_W, WIN_H, Fade(BLACK, 0.55f));
    DrawRectangle((int)px, (int)py, 400, 150, (Color){30, 30, 30, 255});
    DrawRectangleLinesEx((Rectangle){px, py, 400, 150}, 1, GRAY);

    DrawUI("Export PNG", (int)px + 10, (int)py + 10, 16, RAYWHITE);
    DrawUI("Filename (saved to Desktop):", (int)px + 10, (int)py + 30, 12, LIGHTGRAY);

    Rectangle field = {px + 10, py + 48, 380, 30};
    DrawRectangleRec(field, (Color){20, 20, 20, 255});
    DrawRectangleLinesEx(field, 1, (Color){140, 140, 140, 255});
    DrawUI(u->text_input, (int)field.x + 5, (int)field.y + 7, 14, WHITE);

    if ((int)(u->cursor_blink_t * 2) % 2 == 0) {
        int tx = MeasureUI(u->text_input, 14);
        DrawRectangle((int)field.x + 5 + tx, (int)field.y + 5, 2, 20, WHITE);
    }

    if (u->text_len == 0)
        DrawUI("painting", (int)field.x + 6, (int)field.y + 8, 14, (Color){80, 80, 80, 255});

    Rectangle r_ok     = {px + 250, py + 100, 80, 30};
    Rectangle r_cancel = {px + 50,  py + 100, 80, 30};
    ui_button(r_ok, "Export");
    ui_button(r_cancel, "Cancel");
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

void ui_update(UIState *u, Canvas *canvas, sqlite3 *db) {
    u->cursor_blink_t += GetFrameTime();

    switch (u->mode) {
        case UI_SAVE_DIALOG:   save_dialog_update(u, canvas, db); break;
        case UI_LOAD_LIST:     load_list_update(u, canvas, db);   break;
        case UI_CONFIRM_NEW:   confirm_new_update(u, canvas);     break;
        case UI_EXPORT_DIALOG: export_dialog_update(u, canvas);   break;
        default: break;
    }
}

void ui_draw(const UIState *u) {
    switch (u->mode) {
        case UI_SAVE_DIALOG:   save_dialog_draw(u); break;
        case UI_LOAD_LIST:     load_list_draw(u);   break;
        case UI_CONFIRM_NEW:   confirm_new_draw();  break;
        case UI_EXPORT_DIALOG: export_dialog_draw(u); break;
        default: break;
    }
}
