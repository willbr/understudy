#include "toolbar.h"
#include "canvas.h"
#include "font.h"

#include <stdio.h>

// ── Layout constants ──────────────────────────────────────────────────────────
#define TB_W        220
#define TB_H        GetScreenHeight()
#define TB_PAD       10
#define TB_INNER    (TB_W - TB_PAD * 2)

// Sections (y offsets from top)
#define Y_TITLE       10
#define Y_TOOLS       40
#define Y_SIZE_LABEL  90
#define Y_SIZE_SLIDER 108
#define Y_SWATCHES   140
#define Y_RGB_LABEL  310
#define Y_RGB_R      328
#define Y_RGB_G      360
#define Y_RGB_B      392
#define Y_PREVIEW    430
#define Y_BTN_NEW    470
#define Y_BTN_SAVE   505
#define Y_BTN_LOAD   540

// Layer panel
#define Y_LAYER_LABEL  580
#define Y_LAYER_LIST   598
#define LAYER_ROW_H     24
#define MAX_VISIBLE_LAYERS 5
#define Y_LAYER_BTNS   (Y_LAYER_LIST + MAX_VISIBLE_LAYERS * LAYER_ROW_H + 4)

#define SWATCH_COLS  4
#define SWATCH_ROWS  3
#define SWATCH_SIZE  40
#define SWATCH_GAP    5

#define SLIDER_H     16
#define BTN_H        28

// Fixed 12-color palette
static const Color PALETTE[12] = {
    BLACK, DARKGRAY, GRAY, WHITE,
    RED,   ORANGE,   YELLOW, GREEN,
    SKYBLUE, BLUE,   PURPLE, MAROON,
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool button(Rectangle r, const char *label, bool active) {
    Color bg   = active ? DARKBLUE : (Color){60, 60, 60, 255};
    Color text = WHITE;
    bool hovered = CheckCollisionPointRec(GetMousePosition(), r);
    if (hovered) bg = active ? BLUE : (Color){80, 80, 80, 255};
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, (Color){120, 120, 120, 255});
    int fw = MeasureUI(label, 14);
    DrawUI(label, (int)(r.x + r.width / 2 - fw / 2),
             (int)(r.y + r.height / 2 - 7), 14, text);
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static bool small_button(Rectangle r, const char *label) {
    bool hovered = CheckCollisionPointRec(GetMousePosition(), r);
    Color bg = hovered ? (Color){80, 80, 80, 255} : (Color){55, 55, 55, 255};
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, (Color){100, 100, 100, 255});
    int fw = MeasureUI(label, 12);
    DrawUI(label, (int)(r.x + r.width / 2 - fw / 2),
             (int)(r.y + r.height / 2 - 6), 12, WHITE);
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// ── Public API ────────────────────────────────────────────────────────────────

void toolbar_draw(const ToolState *t, const Canvas *c) {
    // Background
    DrawRectangle(0, 0, TB_W, TB_H, (Color){40, 40, 40, 255});
    DrawLine(TB_W, 0, TB_W, TB_H, (Color){70, 70, 70, 255});

    // Title
    DrawUI("Claude Paint", TB_PAD, Y_TITLE, 16, RAYWHITE);

    // Tool buttons
    Rectangle r_brush  = {TB_PAD,          Y_TOOLS, 95, 30};
    Rectangle r_eraser = {TB_PAD + 100,     Y_TOOLS, 95, 30};
    DrawRectangleRec(r_brush,  t->active_tool == TOOL_BRUSH  ? DARKBLUE : (Color){60,60,60,255});
    DrawRectangleLinesEx(r_brush,  1, GRAY);
    DrawUI("Brush",  TB_PAD + 28,        Y_TOOLS + 8, 14, WHITE);
    DrawRectangleRec(r_eraser, t->active_tool == TOOL_ERASER ? DARKBLUE : (Color){60,60,60,255});
    DrawRectangleLinesEx(r_eraser, 1, GRAY);
    DrawUI("Eraser", TB_PAD + 105,       Y_TOOLS + 8, 14, WHITE);

    // Brush size
    char size_label[32];
    snprintf(size_label, sizeof(size_label), "Size: %d", t->brush_radius);
    DrawUI(size_label, TB_PAD, Y_SIZE_LABEL, 13, LIGHTGRAY);
    Rectangle r_size = {TB_PAD, Y_SIZE_SLIDER, TB_INNER, SLIDER_H};
    DrawRectangleRec(r_size, (Color){50, 50, 50, 255});
    float ratio = (float)(t->brush_radius - 1) / 199.0f;
    DrawRectangle((int)r_size.x, (int)r_size.y,
                  (int)(r_size.width * ratio), (int)r_size.height,
                  (Color){100, 140, 200, 255});
    DrawRectangleLinesEx(r_size, 1, (Color){120, 120, 120, 255});

    // Color swatches
    DrawUI("Colors", TB_PAD, Y_SWATCHES - 18, 13, LIGHTGRAY);
    for (int i = 0; i < 12; i++) {
        int col = i % SWATCH_COLS;
        int row = i / SWATCH_COLS;
        Rectangle sr = {
            TB_PAD + col * (SWATCH_SIZE + SWATCH_GAP),
            Y_SWATCHES + row * (SWATCH_SIZE + SWATCH_GAP),
            SWATCH_SIZE, SWATCH_SIZE
        };
        DrawRectangleRec(sr, PALETTE[i]);
        if (t->active_tool == TOOL_BRUSH &&
            t->draw_color.r == PALETTE[i].r &&
            t->draw_color.g == PALETTE[i].g &&
            t->draw_color.b == PALETTE[i].b) {
            DrawRectangleLinesEx(sr, 2, YELLOW);
        } else {
            DrawRectangleLinesEx(sr, 1, (Color){80, 80, 80, 255});
        }
    }

    // Custom RGB sliders
    DrawUI("Custom Color", TB_PAD, Y_RGB_LABEL, 13, LIGHTGRAY);
    Rectangle r_r = {TB_PAD, Y_RGB_R, TB_INNER - 30, SLIDER_H};
    Rectangle r_g = {TB_PAD, Y_RGB_G, TB_INNER - 30, SLIDER_H};
    Rectangle r_b = {TB_PAD, Y_RGB_B, TB_INNER - 30, SLIDER_H};

    DrawRectangleRec(r_r, (Color){50, 50, 50, 255});
    DrawRectangle((int)r_r.x, (int)r_r.y,
                  (int)(r_r.width * t->draw_color.r / 255.0f), SLIDER_H,
                  (Color){200, 60, 60, 255});
    DrawRectangleLinesEx(r_r, 1, GRAY);

    DrawRectangleRec(r_g, (Color){50, 50, 50, 255});
    DrawRectangle((int)r_g.x, (int)r_g.y,
                  (int)(r_g.width * t->draw_color.g / 255.0f), SLIDER_H,
                  (Color){60, 180, 60, 255});
    DrawRectangleLinesEx(r_g, 1, GRAY);

    DrawRectangleRec(r_b, (Color){50, 50, 50, 255});
    DrawRectangle((int)r_b.x, (int)r_b.y,
                  (int)(r_b.width * t->draw_color.b / 255.0f), SLIDER_H,
                  (Color){60, 80, 200, 255});
    DrawRectangleLinesEx(r_b, 1, GRAY);

    char rv[8], gv[8], bv[8];
    snprintf(rv, sizeof(rv), "%3d", t->draw_color.r);
    snprintf(gv, sizeof(gv), "%3d", t->draw_color.g);
    snprintf(bv, sizeof(bv), "%3d", t->draw_color.b);
    DrawUI(rv, TB_W - 30, Y_RGB_R + 1, 12, LIGHTGRAY);
    DrawUI(gv, TB_W - 30, Y_RGB_G + 1, 12, LIGHTGRAY);
    DrawUI(bv, TB_W - 30, Y_RGB_B + 1, 12, LIGHTGRAY);

    // Color preview
    DrawRectangle(TB_PAD, Y_PREVIEW, 40, 30, t->draw_color);
    DrawRectangleLines(TB_PAD, Y_PREVIEW, 40, 30, GRAY);
    DrawUI("Preview", TB_PAD + 48, Y_PREVIEW + 8, 12, LIGHTGRAY);

    // Action buttons
    button((Rectangle){TB_PAD, Y_BTN_NEW,  TB_INNER, BTN_H}, "New Canvas", false);
    button((Rectangle){TB_PAD, Y_BTN_SAVE, TB_INNER, BTN_H}, "Save",       false);
    button((Rectangle){TB_PAD, Y_BTN_LOAD, TB_INNER, BTN_H}, "Load",       false);

    // ── Layer panel ──────────────────────────────────────────────────────────
    DrawUI("Layers", TB_PAD, Y_LAYER_LABEL, 13, LIGHTGRAY);

    // Draw layer rows (bottom layer = index 0 at the bottom of the list)
    int visible = c->layer_count < MAX_VISIBLE_LAYERS ? c->layer_count : MAX_VISIBLE_LAYERS;
    for (int i = 0; i < visible; i++) {
        // Show layers top-down: highest index at top of list
        int li = c->layer_count - 1 - i;
        float ry = Y_LAYER_LIST + i * LAYER_ROW_H;
        Rectangle row = {TB_PAD, ry, TB_INNER, LAYER_ROW_H - 2};

        Color bg = (li == c->active_layer)
                   ? (Color){50, 70, 120, 255}
                   : (Color){50, 50, 50, 255};
        DrawRectangleRec(row, bg);
        DrawRectangleLinesEx(row, 1, (Color){70, 70, 70, 255});

        // Visibility indicator
        const char *vis = c->layers[li].visible ? "O" : "-";
        DrawUI(vis, (int)row.x + 4, (int)ry + 5, 12,
                 c->layers[li].visible ? GREEN : (Color){80, 80, 80, 255});

        // Layer name
        DrawUI(c->layers[li].name, (int)row.x + 20, (int)ry + 5, 12, WHITE);
    }

    // Layer buttons
    float by = Y_LAYER_BTNS;
    float bw = (TB_INNER - 12) / 4.0f;
    small_button((Rectangle){TB_PAD,             by, bw, 20}, "+");
    small_button((Rectangle){TB_PAD + bw + 4,    by, bw, 20}, "-");
    small_button((Rectangle){TB_PAD + 2*(bw+4),  by, bw, 20}, "^");
    small_button((Rectangle){TB_PAD + 3*(bw+4),  by, bw, 20}, "v");
}

ToolbarEvents toolbar_update(ToolState *t, Canvas *c) {
    ToolbarEvents ev = {false, false, false};
    Vector2 mouse = GetMousePosition();
    bool ldown    = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    bool lpress   = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

    if (mouse.x > TB_W) return ev;

    // Tool buttons
    Rectangle r_brush  = {TB_PAD,      Y_TOOLS, 95, 30};
    Rectangle r_eraser = {TB_PAD + 100, Y_TOOLS, 95, 30};
    if (lpress && CheckCollisionPointRec(mouse, r_brush))  t->active_tool = TOOL_BRUSH;
    if (lpress && CheckCollisionPointRec(mouse, r_eraser)) t->active_tool = TOOL_ERASER;

    // Brush size slider
    Rectangle r_size = {TB_PAD, Y_SIZE_SLIDER, TB_INNER, SLIDER_H};
    if (ldown && CheckCollisionPointRec(mouse, r_size)) {
        float fv = (mouse.x - r_size.x) / r_size.width * 199.0f + 1.0f;
        t->brush_radius = (int)(fv + 0.5f);
        if (t->brush_radius < 1)  t->brush_radius = 1;
        if (t->brush_radius > 200) t->brush_radius = 200;
    }

    // Color swatches
    for (int i = 0; i < 12; i++) {
        int col = i % SWATCH_COLS;
        int row = i / SWATCH_COLS;
        Rectangle sr = {
            TB_PAD + col * (SWATCH_SIZE + SWATCH_GAP),
            Y_SWATCHES + row * (SWATCH_SIZE + SWATCH_GAP),
            SWATCH_SIZE, SWATCH_SIZE
        };
        if (lpress && CheckCollisionPointRec(mouse, sr)) {
            t->draw_color  = PALETTE[i];
            t->active_tool = TOOL_BRUSH;
        }
    }

    // Custom RGB sliders
    Rectangle r_r = {TB_PAD, Y_RGB_R, TB_INNER - 30, SLIDER_H};
    Rectangle r_g = {TB_PAD, Y_RGB_G, TB_INNER - 30, SLIDER_H};
    Rectangle r_b = {TB_PAD, Y_RGB_B, TB_INNER - 30, SLIDER_H};

    if (ldown && CheckCollisionPointRec(mouse, r_r)) {
        float v = (mouse.x - r_r.x) / r_r.width * 255.0f;
        t->draw_color.r = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        t->active_tool  = TOOL_BRUSH;
    }
    if (ldown && CheckCollisionPointRec(mouse, r_g)) {
        float v = (mouse.x - r_g.x) / r_g.width * 255.0f;
        t->draw_color.g = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        t->active_tool  = TOOL_BRUSH;
    }
    if (ldown && CheckCollisionPointRec(mouse, r_b)) {
        float v = (mouse.x - r_b.x) / r_b.width * 255.0f;
        t->draw_color.b = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        t->active_tool  = TOOL_BRUSH;
    }

    // Action buttons
    if (lpress && CheckCollisionPointRec(mouse, (Rectangle){TB_PAD, Y_BTN_NEW,  TB_INNER, BTN_H}))
        ev.wants_new  = true;
    if (lpress && CheckCollisionPointRec(mouse, (Rectangle){TB_PAD, Y_BTN_SAVE, TB_INNER, BTN_H}))
        ev.wants_save = true;
    if (lpress && CheckCollisionPointRec(mouse, (Rectangle){TB_PAD, Y_BTN_LOAD, TB_INNER, BTN_H}))
        ev.wants_load = true;

    // ── Layer panel interaction ──────────────────────────────────────────────
    if (lpress) {
        int visible = c->layer_count < MAX_VISIBLE_LAYERS ? c->layer_count : MAX_VISIBLE_LAYERS;
        for (int i = 0; i < visible; i++) {
            int li = c->layer_count - 1 - i;
            float ry = Y_LAYER_LIST + i * LAYER_ROW_H;
            Rectangle row = {TB_PAD, ry, TB_INNER, LAYER_ROW_H - 2};
            if (CheckCollisionPointRec(mouse, row)) {
                // Click on visibility toggle area (first 18px)
                if (mouse.x < TB_PAD + 18) {
                    canvas_toggle_layer_visible(c, li);
                } else {
                    canvas_set_active_layer(c, li);
                }
            }
        }

        // Layer buttons: + - ^ v
        float btn_y = Y_LAYER_BTNS;
        float bw = (TB_INNER - 12) / 4.0f;
        Rectangle r_add  = {TB_PAD,             btn_y, bw, 20};
        Rectangle r_del  = {TB_PAD + bw + 4,    btn_y, bw, 20};
        Rectangle r_up   = {TB_PAD + 2*(bw+4),  btn_y, bw, 20};
        Rectangle r_down = {TB_PAD + 3*(bw+4),  btn_y, bw, 20};

        if (CheckCollisionPointRec(mouse, r_add))
            canvas_add_layer(c);
        if (CheckCollisionPointRec(mouse, r_del))
            canvas_delete_layer(c, c->active_layer);
        if (CheckCollisionPointRec(mouse, r_up))
            canvas_move_layer(c, c->active_layer, c->active_layer + 1);
        if (CheckCollisionPointRec(mouse, r_down))
            canvas_move_layer(c, c->active_layer, c->active_layer - 1);
    }

    return ev;
}
