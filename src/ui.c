#include "ui.h"
#include "font.h"
#include "refimage.h"

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
            int nrefs = refimage_count();
            RefImage *refs_arr = nrefs > 0 ? malloc(nrefs * sizeof(RefImage)) : NULL;
            for (int i = 0; i < nrefs; i++) refs_arr[i] = *refimage_get(i);
            db_save_painting(db, u->text_input,
                             canvas->layers, canvas->layer_count,
                             refs_arr, nrefs,
                             canvas->width, canvas->height);
            free(refs_arr);
            canvas->dirty = false;
            strncpy(u->last_saved_name, u->text_input, 127);
            u->last_saved_name[127] = '\0';
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
    Rectangle r_rename = {px + 100, py + 420, 80, 30};

    // Inline rename mode
    if (u->load_renaming) {
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            if (u->text_len < 127) {
                u->text_input[u->text_len++] = (char)ch;
                u->text_input[u->text_len]   = '\0';
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) && u->text_len > 0)
            u->text_input[--u->text_len] = '\0';

        if (IsKeyPressed(KEY_ENTER) && u->text_len > 0) {
            db_rename_painting(db, u->load_list[u->load_selected].id, u->text_input);
            db_free_list(u->load_list);
            u->load_list = NULL;
            db_list_paintings(db, &u->load_list, &u->load_count);
            u->load_renaming = false;
        }
        if (IsKeyPressed(KEY_ESCAPE))
            u->load_renaming = false;
        return;
    }

    if (ui_button(r_open, "Open") && u->load_selected >= 0 &&
        u->load_selected < u->load_count) {
        PaintingMeta *m = &u->load_list[u->load_selected];
        Layer *layers = NULL;
        int lcount = 0;
        RefImage *refs_loaded = NULL;
        int rc = 0;
        int w = 0, h = 0;
        if (db_load_painting(db, m->id, &layers, &lcount, &refs_loaded, &rc, &w, &h)) {
            canvas_load_layers(canvas, layers, lcount);
            refimage_load_from_db(refs_loaded, rc);
            // Bump next_z to account for loaded refs
            for (int ri = 0; ri < rc; ri++) {
                RefImage *r = refimage_get(ri);
                if (r) canvas_bump_next_z_to(canvas, r->z);
            }
            strncpy(u->last_saved_name, m->name, 127);
            u->last_saved_name[127] = '\0';
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
        db_free_list(u->load_list);
        u->load_list = NULL;
        db_list_paintings(db, &u->load_list, &u->load_count);
        if (u->load_selected >= u->load_count)
            u->load_selected = u->load_count - 1;
    }

    if (ui_button(r_rename, "Rename") && u->load_selected >= 0 &&
        u->load_selected < u->load_count) {
        // Pre-fill with current name
        strncpy(u->text_input, u->load_list[u->load_selected].name, 127);
        u->text_input[127] = '\0';
        u->text_len = (int)strlen(u->text_input);
        u->load_renaming = true;
        u->cursor_blink_t = 0.0f;
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

    // Inline rename field
    if (u->load_renaming) {
        Rectangle field = {px + 10, py + 420, 380, 28};
        DrawRectangleRec(field, (Color){20, 20, 20, 255});
        DrawRectangleLinesEx(field, 1, (Color){100, 160, 255, 255});
        DrawUI(u->text_input, (int)field.x + 5, (int)field.y + 6, 14, WHITE);
        if ((int)(u->cursor_blink_t * 2) % 2 == 0) {
            int tx = MeasureUI(u->text_input, 14);
            DrawRectangle((int)field.x + 5 + tx, (int)field.y + 5, 2, 18, WHITE);
        }
        DrawUI("Enter to confirm, Esc to cancel", (int)px + 10, (int)py + 452, 11, (Color){80,80,80,255});
    } else {
        Rectangle r_open   = {px + 390, py + 420, 80, 30};
        Rectangle r_cancel = {px + 280, py + 420, 80, 30};
        Rectangle r_delete = {px + 10,  py + 420, 80, 30};
        Rectangle r_rename = {px + 100, py + 420, 80, 30};
        ui_button(r_open, "Open");
        ui_button(r_cancel, "Cancel");
        ui_button(r_delete, "Delete");
        ui_button(r_rename, "Rename");
    }
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

    float dw = 340, dh = 190;
    float px = WIN_W / 2.0f - dw / 2;
    float py = WIN_H / 2.0f - dh / 2;
    float pad = 20;
    float bw = 50, bh = 28, gap = 6;

    // Scale selector buttons
    float scale_y = py + 100;
    float scale_x = px + pad;
    Rectangle r_1x = {scale_x,               scale_y, bw, bh};
    Rectangle r_2x = {scale_x + (bw + gap),  scale_y, bw, bh};
    Rectangle r_4x = {scale_x + 2*(bw+gap),  scale_y, bw, bh};
    Rectangle r_8x = {scale_x + 3*(bw+gap),  scale_y, bw, bh};
    if (ui_button(r_1x, "1x")) u->export_scale = 1;
    if (ui_button(r_2x, "2x")) u->export_scale = 2;
    if (ui_button(r_4x, "4x")) u->export_scale = 4;
    if (ui_button(r_8x, "8x")) u->export_scale = 8;

    float btn_w = 90, btn_h = 32;
    float btn_y = py + dh - pad - btn_h;
    Rectangle r_cancel = {px + pad, btn_y, btn_w, btn_h};
    Rectangle r_ok     = {px + dw - pad - btn_w, btn_y, btn_w, btn_h};

    if (IsKeyPressed(KEY_ENTER) || ui_button(r_ok, "Export")) {
        if (u->text_len > 0) {
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
            canvas_export_png(canvas, path, u->export_scale);
            u->mode = UI_NONE;
        }
    }
    if (IsKeyPressed(KEY_ESCAPE) || ui_button(r_cancel, "Cancel")) {
        u->mode = UI_NONE;
    }
}

static void export_dialog_draw(const UIState *u) {
    float dw = 340, dh = 190;
    float px = WIN_W / 2.0f - dw / 2;
    float py = WIN_H / 2.0f - dh / 2;
    float pad = 20;
    float bw = 50, bh = 28, gap = 6;

    DrawRectangle(0, 0, WIN_W, WIN_H, Fade(BLACK, 0.55f));
    DrawRectangleRounded((Rectangle){px, py, dw, dh}, 0.04f, 8, (Color){30, 30, 30, 245});
    DrawRectangleRoundedLinesEx((Rectangle){px, py, dw, dh}, 0.04f, 8, 1, (Color){70, 70, 70, 255});

    // Title
    DrawUI("Export PNG", (int)(px + pad), (int)(py + pad), 16, RAYWHITE);

    // Filename field
    float field_y = py + pad + 24;
    DrawUI("Filename", (int)(px + pad), (int)field_y, 11, (Color){140, 140, 140, 255});
    Rectangle field = {px + pad, field_y + 16, dw - 2 * pad, 28};
    DrawRectangleRounded(field, 0.15f, 4, (Color){15, 15, 15, 255});
    DrawRectangleRoundedLinesEx(field, 0.15f, 4, 1, (Color){100, 100, 100, 255});
    DrawUI(u->text_input, (int)field.x + 8, (int)field.y + 7, 14, WHITE);

    if ((int)(u->cursor_blink_t * 2) % 2 == 0) {
        int tx = MeasureUI(u->text_input, 14);
        DrawRectangle((int)field.x + 8 + tx, (int)field.y + 5, 2, 18, WHITE);
    }

    if (u->text_len == 0)
        DrawUI("painting", (int)field.x + 9, (int)field.y + 7, 14, (Color){80, 80, 80, 255});

    // Scale selector
    float scale_y = py + 100;
    float scale_x = px + pad;
    DrawUI("Scale", (int)scale_x, (int)(scale_y - 14), 11, (Color){140, 140, 140, 255});
    Rectangle r_1x = {scale_x,               scale_y, bw, bh};
    Rectangle r_2x = {scale_x + (bw + gap),  scale_y, bw, bh};
    Rectangle r_4x = {scale_x + 2*(bw+gap),  scale_y, bw, bh};
    Rectangle r_8x = {scale_x + 3*(bw+gap),  scale_y, bw, bh};
    // Highlight active scale
    if (u->export_scale == 1) DrawRectangleRec(r_1x, DARKBLUE);
    if (u->export_scale == 2) DrawRectangleRec(r_2x, DARKBLUE);
    if (u->export_scale == 4) DrawRectangleRec(r_4x, DARKBLUE);
    if (u->export_scale == 8) DrawRectangleRec(r_8x, DARKBLUE);
    ui_button(r_1x, "1x");
    ui_button(r_2x, "2x");
    ui_button(r_4x, "4x");
    ui_button(r_8x, "8x");

    // Action buttons
    float btn_w = 90, btn_h = 32;
    float btn_y = py + dh - pad - btn_h;
    Rectangle r_cancel = {px + pad, btn_y, btn_w, btn_h};
    Rectangle r_ok     = {px + dw - pad - btn_w, btn_y, btn_w, btn_h};
    ui_button(r_cancel, "Cancel");
    ui_button(r_ok, "Export");
}

// ── Crop Mode ─────────────────────────────────────────────────────────────────

static void crop_mode_update(UIState *u, Canvas *canvas, int canvas_x) {
    Vector2 mouse = GetMousePosition();
    float doc_x = (mouse.x - canvas_x - canvas->view_x) / canvas->zoom;
    float doc_y = (mouse.y - CANVAS_Y - canvas->view_y) / canvas->zoom;

    if (IsKeyPressed(KEY_ESCAPE)) {
        u->mode = UI_NONE;
        return;
    }

    // Apply/Cancel buttons (checked before drag so clicks don't start a new rect)
    if (u->crop_rect_valid) {
        Rectangle r_apply  = {(float)(WIN_W / 2 - 90), (float)(WIN_H - 50), 80, 30};
        Rectangle r_cancel = {(float)(WIN_W / 2 + 10), (float)(WIN_H - 50), 80, 30};
        if (ui_button(r_apply, "Apply")) {
            int cx = (int)u->crop_start.x;
            int cy = (int)u->crop_start.y;
            int cw = (int)(u->crop_end.x - u->crop_start.x);
            int ch = (int)(u->crop_end.y - u->crop_start.y);
            canvas_crop(canvas, cx, cy, cw, ch);
            u->mode = UI_NONE;
            return;
        }
        if (ui_button(r_cancel, "Cancel")) {
            u->mode = UI_NONE;
            return;
        }
        // Don't start new drag if clicking on buttons
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            (CheckCollisionPointRec(mouse, r_apply) || CheckCollisionPointRec(mouse, r_cancel)))
            return;
    }

    // Drag to define crop rectangle
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && mouse.x >= canvas_x) {
        u->crop_start = (Vector2){doc_x, doc_y};
        u->crop_end   = u->crop_start;
        u->crop_dragging = true;
        u->crop_rect_valid = false;
    }
    if (u->crop_dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        u->crop_end = (Vector2){doc_x, doc_y};
    }
    if (u->crop_dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        u->crop_dragging = false;
        float w = u->crop_end.x - u->crop_start.x;
        float h = u->crop_end.y - u->crop_start.y;
        if (w < 0) { float t = u->crop_start.x; u->crop_start.x = u->crop_end.x; u->crop_end.x = t; }
        if (h < 0) { float t = u->crop_start.y; u->crop_start.y = u->crop_end.y; u->crop_end.y = t; }
        w = u->crop_end.x - u->crop_start.x;
        h = u->crop_end.y - u->crop_start.y;
        u->crop_rect_valid = (w > 1 && h > 1);
    }
}

static void crop_mode_draw(const UIState *u, const Canvas *c, int canvas_x) {
    // Draw instruction text
    DrawUI("Drag to select crop area", canvas_x + 10, 10, 14, WHITE);

    if (!u->crop_rect_valid && !u->crop_dragging) return;

    // Convert crop rect from doc space to screen space
    float sx1 = u->crop_start.x * c->zoom + c->view_x + canvas_x;
    float sy1 = u->crop_start.y * c->zoom + c->view_y;
    float sx2 = u->crop_end.x   * c->zoom + c->view_x + canvas_x;
    float sy2 = u->crop_end.y   * c->zoom + c->view_y;
    if (sx1 > sx2) { float t = sx1; sx1 = sx2; sx2 = t; }
    if (sy1 > sy2) { float t = sy1; sy1 = sy2; sy2 = t; }

    float rw = sx2 - sx1, rh = sy2 - sy1;

    // Dim area outside crop
    DrawRectangle(0, 0, WIN_W, (int)sy1, Fade(BLACK, 0.4f));
    DrawRectangle(0, (int)sy2, WIN_W, WIN_H - (int)sy2, Fade(BLACK, 0.4f));
    DrawRectangle(0, (int)sy1, (int)sx1, (int)rh, Fade(BLACK, 0.4f));
    DrawRectangle((int)sx2, (int)sy1, WIN_W - (int)sx2, (int)rh, Fade(BLACK, 0.4f));

    // Crop border
    DrawRectangleLinesEx((Rectangle){sx1, sy1, rw, rh}, 2.0f,
                         (Color){255, 200, 50, 255});

    // Size label
    int cw = (int)(u->crop_end.x - u->crop_start.x);
    int ch_val = (int)(u->crop_end.y - u->crop_start.y);
    if (cw < 0) cw = -cw;
    if (ch_val < 0) ch_val = -ch_val;
    char sz[32];
    snprintf(sz, sizeof(sz), "%d x %d", cw, ch_val);
    DrawUI(sz, (int)sx1 + 4, (int)sy1 - 18, 13, (Color){255, 200, 50, 255});

    // Buttons
    if (u->crop_rect_valid) {
        Rectangle r_apply  = {(float)(WIN_W / 2 - 90), (float)(WIN_H - 50), 80, 30};
        Rectangle r_cancel = {(float)(WIN_W / 2 + 10), (float)(WIN_H - 50), 80, 30};
        ui_button(r_apply, "Apply");
        ui_button(r_cancel, "Cancel");
    }
}

// ── Resize Dialog ─────────────────────────────────────────────────────────────

static void resize_dialog_update(UIState *u, Canvas *canvas) {
    // Route keyboard input to active field
    char *buf;
    int  *len;
    if (u->resize_active_field == 0) {
        buf = u->resize_w_buf; len = &u->resize_w_len;
    } else {
        buf = u->resize_h_buf; len = &u->resize_h_len;
    }

    int ch;
    while ((ch = GetCharPressed()) != 0) {
        if (ch >= '0' && ch <= '9' && *len < 6) {
            buf[(*len)++] = (char)ch;
            buf[*len] = '\0';
            // Auto-update other field if locked
            if (u->resize_lock_aspect) {
                int val = atoi(buf);
                if (u->resize_active_field == 0) {
                    int h = (int)(val / u->resize_aspect + 0.5f);
                    snprintf(u->resize_h_buf, 16, "%d", h);
                    u->resize_h_len = (int)strlen(u->resize_h_buf);
                } else {
                    int w = (int)(val * u->resize_aspect + 0.5f);
                    snprintf(u->resize_w_buf, 16, "%d", w);
                    u->resize_w_len = (int)strlen(u->resize_w_buf);
                }
            }
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) && *len > 0) {
        buf[--(*len)] = '\0';
        if (u->resize_lock_aspect && *len > 0) {
            int val = atoi(buf);
            if (u->resize_active_field == 0) {
                int h = (int)(val / u->resize_aspect + 0.5f);
                snprintf(u->resize_h_buf, 16, "%d", h);
                u->resize_h_len = (int)strlen(u->resize_h_buf);
            } else {
                int w = (int)(val * u->resize_aspect + 0.5f);
                snprintf(u->resize_w_buf, 16, "%d", w);
                u->resize_w_len = (int)strlen(u->resize_w_buf);
            }
        }
    }
    if (IsKeyPressed(KEY_TAB))
        u->resize_active_field = 1 - u->resize_active_field;

    float px = WIN_W / 2.0f - 180;
    float py = WIN_H / 2.0f - 100;

    // Field click detection
    Rectangle r_wfield = {px + 40, py + 60, 120, 28};
    Rectangle r_hfield = {px + 210, py + 60, 120, 28};
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec(GetMousePosition(), r_wfield))
            u->resize_active_field = 0;
        if (CheckCollisionPointRec(GetMousePosition(), r_hfield))
            u->resize_active_field = 1;
    }

    // Lock toggle
    Rectangle r_lock = {px + 165, py + 63, 40, 22};
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(GetMousePosition(), r_lock)) {
        u->resize_lock_aspect = !u->resize_lock_aspect;
    }

    // Half / Double buttons
    Rectangle r_half   = {px + 80,  py + 100, 80, 26};
    Rectangle r_double = {px + 200, py + 100, 80, 26};
    if (ui_button(r_half, "Half")) {
        int nw = atoi(u->resize_w_buf) / 2;
        int nh = atoi(u->resize_h_buf) / 2;
        if (nw < 64) nw = 64;
        if (nh < 64) nh = 64;
        snprintf(u->resize_w_buf, 16, "%d", nw);
        u->resize_w_len = (int)strlen(u->resize_w_buf);
        snprintf(u->resize_h_buf, 16, "%d", nh);
        u->resize_h_len = (int)strlen(u->resize_h_buf);
    }
    if (ui_button(r_double, "Double")) {
        int nw = atoi(u->resize_w_buf) * 2;
        int nh = atoi(u->resize_h_buf) * 2;
        if (nw > 16384) nw = 16384;
        if (nh > 16384) nh = 16384;
        snprintf(u->resize_w_buf, 16, "%d", nw);
        u->resize_w_len = (int)strlen(u->resize_w_buf);
        snprintf(u->resize_h_buf, 16, "%d", nh);
        u->resize_h_len = (int)strlen(u->resize_h_buf);
    }

    Rectangle r_apply  = {px + 200, py + 155, 80, 30};
    Rectangle r_cancel = {px + 80,  py + 155, 80, 30};

    if (IsKeyPressed(KEY_ENTER) || ui_button(r_apply, "Apply")) {
        int new_w = atoi(u->resize_w_buf);
        int new_h = atoi(u->resize_h_buf);
        if (new_w >= 64 && new_h >= 64) {
            canvas_resize_doc(canvas, new_w, new_h);
            u->mode = UI_NONE;
        }
    }
    if (IsKeyPressed(KEY_ESCAPE) || ui_button(r_cancel, "Cancel")) {
        u->mode = UI_NONE;
    }
}

static void resize_dialog_draw(const UIState *u) {
    float px = WIN_W / 2.0f - 180;
    float py = WIN_H / 2.0f - 100;

    DrawRectangle(0, 0, WIN_W, WIN_H, Fade(BLACK, 0.55f));
    DrawRectangle((int)px, (int)py, 360, 210, (Color){30, 30, 30, 255});
    DrawRectangleLinesEx((Rectangle){px, py, 360, 210}, 1, GRAY);

    DrawUI("Resize Document", (int)px + 10, (int)py + 10, 16, RAYWHITE);

    char cur[64];
    snprintf(cur, sizeof(cur), "Current: %s x %s", u->resize_w_buf, u->resize_h_buf);
    DrawUI("Width:", (int)px + 10, (int)py + 45, 12, LIGHTGRAY);
    DrawUI("Height:", (int)px + 180, (int)py + 45, 12, LIGHTGRAY);

    // Width field
    Rectangle r_wfield = {px + 40, py + 60, 120, 28};
    DrawRectangleRec(r_wfield, (Color){20, 20, 20, 255});
    DrawRectangleLinesEx(r_wfield, 1,
        u->resize_active_field == 0 ? (Color){100, 160, 255, 255} : (Color){100, 100, 100, 255});
    DrawUI(u->resize_w_buf, (int)r_wfield.x + 5, (int)r_wfield.y + 6, 14, WHITE);

    // Lock toggle
    Rectangle r_lock = {px + 165, py + 63, 40, 22};
    DrawRectangleRec(r_lock, u->resize_lock_aspect ? (Color){60, 100, 160, 255} : (Color){50, 50, 50, 255});
    DrawRectangleLinesEx(r_lock, 1, (Color){100, 100, 100, 255});
    DrawUI(u->resize_lock_aspect ? "L" : "U", (int)r_lock.x + 15, (int)r_lock.y + 4, 12, WHITE);

    // Height field
    Rectangle r_hfield = {px + 210, py + 60, 120, 28};
    DrawRectangleRec(r_hfield, (Color){20, 20, 20, 255});
    DrawRectangleLinesEx(r_hfield, 1,
        u->resize_active_field == 1 ? (Color){100, 160, 255, 255} : (Color){100, 100, 100, 255});
    DrawUI(u->resize_h_buf, (int)r_hfield.x + 5, (int)r_hfield.y + 6, 14, WHITE);

    // Half / Double buttons
    Rectangle r_half   = {px + 80,  py + 100, 80, 26};
    Rectangle r_double = {px + 200, py + 100, 80, 26};
    ui_button(r_half, "Half");
    ui_button(r_double, "Double");

    // Help text
    DrawUI("Min 64, Max 16384. Tab to switch fields.", (int)px + 10, (int)py + 132, 11, (Color){100, 100, 100, 255});

    Rectangle r_apply  = {px + 200, py + 155, 80, 30};
    Rectangle r_cancel = {px + 80,  py + 155, 80, 30};
    ui_button(r_apply, "Apply");
    ui_button(r_cancel, "Cancel");
}

// ── Help Overlay ──────────────────────────────────────────────────────────────

static void help_update(UIState *u) {
    if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        u->mode = UI_NONE;
}

static void help_draw(void) {
    DrawRectangle(0, 0, WIN_W, WIN_H, Fade(BLACK, 0.7f));

    float px = WIN_W / 2.0f - 160;
    float py = WIN_H / 2.0f - 180;
    DrawRectangle((int)px, (int)py, 320, 360, (Color){30, 30, 30, 255});
    DrawRectangleLinesEx((Rectangle){px, py, 320, 360}, 1, GRAY);

    DrawUI("Keyboard Shortcuts", (int)px + 10, (int)py + 10, 16, RAYWHITE);

    int y = (int)py + 40;
    int lx = (int)px + 16;
    int rx = (int)px + 120;
    int gap = 22;

    DrawUI("Space",    lx, y, 13, (Color){120, 180, 255, 255});
    DrawUI("Pan canvas",       rx, y, 13, LIGHTGRAY); y += gap;
    DrawUI("Scroll",   lx, y, 13, (Color){120, 180, 255, 255});
    DrawUI("Zoom",             rx, y, 13, LIGHTGRAY); y += gap;
    DrawUI("G + drag", lx, y, 13, (Color){120, 180, 255, 255});
    DrawUI("Scrub zoom",       rx, y, 13, LIGHTGRAY); y += gap;
    DrawUI("D + drag", lx, y, 13, (Color){120, 180, 255, 255});
    DrawUI("Scrub brush size", rx, y, 13, LIGHTGRAY); y += gap;
    DrawUI("F (hold)", lx, y, 13, (Color){120, 180, 255, 255});
    DrawUI("Color picker",     rx, y, 13, LIGHTGRAY); y += gap;
    DrawUI("E + click",lx, y, 13, (Color){120, 180, 255, 255});
    DrawUI("Eyedropper",       rx, y, 13, LIGHTGRAY); y += gap;
    DrawUI("Cmd/Ctrl+Z",lx, y, 13, (Color){120, 180, 255, 255});
    DrawUI("Undo",             rx, y, 13, LIGHTGRAY); y += gap;
    DrawUI("Tab",      lx, y, 13, (Color){120, 180, 255, 255});
    DrawUI("Toggle toolbar",   rx, y, 13, LIGHTGRAY); y += gap;
    DrawUI("?",        lx, y, 13, (Color){120, 180, 255, 255});
    DrawUI("This help",        rx, y, 13, LIGHTGRAY); y += gap + 10;

    DrawUI("Click or Esc to close", (int)px + 70, y, 12, (Color){80, 80, 80, 255});
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

void ui_update(UIState *u, Canvas *canvas, sqlite3 *db, int canvas_x) {
    u->cursor_blink_t += GetFrameTime();

    switch (u->mode) {
        case UI_SAVE_DIALOG:   save_dialog_update(u, canvas, db); break;
        case UI_LOAD_LIST:     load_list_update(u, canvas, db);   break;
        case UI_EXPORT_DIALOG: export_dialog_update(u, canvas);   break;
        case UI_CROP_MODE:     crop_mode_update(u, canvas, canvas_x); break;
        case UI_RESIZE_DIALOG: resize_dialog_update(u, canvas);   break;
        case UI_HELP:          help_update(u); break;
        default: break;
    }
}

void ui_draw(const UIState *u, const Canvas *c, int canvas_x) {
    switch (u->mode) {
        case UI_SAVE_DIALOG:   save_dialog_draw(u); break;
        case UI_LOAD_LIST:     load_list_draw(u);   break;
        case UI_EXPORT_DIALOG: export_dialog_draw(u); break;
        case UI_CROP_MODE:     crop_mode_draw(u, c, canvas_x); break;
        case UI_RESIZE_DIALOG: resize_dialog_draw(u); break;
        case UI_HELP:          help_draw(); break;
        default: break;
    }
}
