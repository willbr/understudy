#include "toolbar.h"
#include "canvas.h"
#include "font.h"
#include "rlgl.h"
#include "refimage.h"

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
#define Y_BTN_EXPORT 575
#define Y_BTN_CROP   610
#define Y_BTN_RESIZE 645

// Layer panel
#define Y_LAYER_LABEL  685
#define Y_LAYER_LIST   703
#define LAYER_ROW_H     24
#define MAX_VISIBLE_LAYERS 5
#define Y_LAYER_BTNS   (Y_LAYER_LIST + MAX_VISIBLE_LAYERS * LAYER_ROW_H + 4)

// References panel
#define REF_ROW_H      24
#define MAX_VISIBLE_REFS 4
#define Y_REF_LABEL    (Y_LAYER_BTNS + 32)
#define Y_REF_LIST     (Y_REF_LABEL + 18)

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

// ── Scroll state ─────────────────────────────────────────────────────────────
static int tb_scroll = 0;
static int layer_scroll = 0;  // scroll offset for layer list
static int ref_scroll = 0;    // scroll offset for refs list

// Draw a vertical scrollbar. Returns true if content is scrolled.
static void draw_scrollbar(float x, float y, float height,
                           int scroll, int content_h, int visible_h) {
    if (content_h <= visible_h) return;  // no scrollbar needed

    float bar_w = 4;
    float track_h = height;
    float thumb_h = (float)visible_h / (float)content_h * track_h;
    if (thumb_h < 16) thumb_h = 16;
    float thumb_y = y + (float)scroll / (float)(content_h - visible_h) * (track_h - thumb_h);

    // Track
    DrawRectangle((int)x, (int)y, (int)bar_w, (int)track_h,
                  (Color){30, 30, 30, 150});
    // Thumb
    DrawRectangleRounded((Rectangle){x, thumb_y, bar_w, thumb_h}, 0.5f, 4,
                         (Color){120, 120, 120, 200});
}

// ── Public API ────────────────────────────────────────────────────────────────

void toolbar_draw(const ToolState *t, const Canvas *c) {
    // Background
    DrawRectangle(0, 0, TB_W, TB_H, (Color){40, 40, 40, 255});
    DrawLine(TB_W, 0, TB_W, TB_H, (Color){70, 70, 70, 255});

    BeginScissorMode(0, 0, TB_W, TB_H);
    rlPushMatrix();
    rlTranslatef(0, -(float)tb_scroll, 0);

    // Title
    DrawUI("Claude Paint", TB_PAD, Y_TITLE, 16, RAYWHITE);

    // Tool buttons
    Rectangle r_brush  = {TB_PAD,          Y_TOOLS, 60, 30};
    Rectangle r_eraser = {TB_PAD + 65,     Y_TOOLS, 65, 30};
    Rectangle r_line   = {TB_PAD + 135,    Y_TOOLS, 60, 30};
    DrawRectangleRec(r_brush,  t->active_tool == TOOL_BRUSH  ? DARKBLUE : (Color){60,60,60,255});
    DrawRectangleLinesEx(r_brush,  1, GRAY);
    DrawUI("Brush",  TB_PAD + 12,        Y_TOOLS + 8, 13, WHITE);
    DrawRectangleRec(r_eraser, t->active_tool == TOOL_ERASER ? DARKBLUE : (Color){60,60,60,255});
    DrawRectangleLinesEx(r_eraser, 1, GRAY);
    DrawUI("Eraser", TB_PAD + 72,        Y_TOOLS + 8, 13, WHITE);
    DrawRectangleRec(r_line,   t->active_tool == TOOL_LINE   ? DARKBLUE : (Color){60,60,60,255});
    DrawRectangleLinesEx(r_line, 1, GRAY);
    DrawUI("Line",   TB_PAD + 147,       Y_TOOLS + 8, 13, WHITE);

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
    button((Rectangle){TB_PAD, Y_BTN_LOAD,   TB_INNER, BTN_H}, "Load",       false);
    button((Rectangle){TB_PAD, Y_BTN_EXPORT, TB_INNER, BTN_H}, "Export PNG", false);
    button((Rectangle){TB_PAD, Y_BTN_CROP,   TB_INNER, BTN_H}, "Crop",       false);
    button((Rectangle){TB_PAD, Y_BTN_RESIZE, TB_INNER, BTN_H}, "Resize",     false);

    // ── Layer panel ──────────────────────────────────────────────────────────
    {
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "Layers (%d)", c->layer_count);
        DrawUI(lbl, TB_PAD, Y_LAYER_LABEL, 13, LIGHTGRAY);
    }

    // Clamp layer scroll
    int max_layer_scroll = c->layer_count - MAX_VISIBLE_LAYERS;
    if (max_layer_scroll < 0) max_layer_scroll = 0;
    if (layer_scroll > max_layer_scroll) layer_scroll = max_layer_scroll;
    if (layer_scroll < 0) layer_scroll = 0;

    // Draw layer rows (bottom layer = index 0 at the bottom of the list)
    int visible = c->layer_count < MAX_VISIBLE_LAYERS ? c->layer_count : MAX_VISIBLE_LAYERS;
    for (int i = 0; i < visible; i++) {
        // Show layers top-down: highest index at top of list, with scroll offset
        int li = c->layer_count - 1 - i - layer_scroll;
        if (li < 0 || li >= c->layer_count) continue;
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

    // Scroll indicator
    if (c->layer_count > MAX_VISIBLE_LAYERS) {
        char sc[16];
        snprintf(sc, sizeof(sc), "%d-%d/%d", layer_scroll + 1,
                 layer_scroll + visible, c->layer_count);
        DrawUI(sc, TB_PAD + TB_INNER - 60, Y_LAYER_LABEL, 11, (Color){80, 80, 80, 255});
    }

    // Layer buttons
    float by = Y_LAYER_BTNS;
    float bw = (TB_INNER - 12) / 4.0f;
    small_button((Rectangle){TB_PAD,             by, bw, 20}, "+");
    small_button((Rectangle){TB_PAD + bw + 4,    by, bw, 20}, "-");
    small_button((Rectangle){TB_PAD + 2*(bw+4),  by, bw, 20}, "^");
    small_button((Rectangle){TB_PAD + 3*(bw+4),  by, bw, 20}, "v");

    // ── References panel ─────────────────────────────────────────────────────
    {
        int n_refs = refimage_count();
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "References (%d)", n_refs);
        DrawUI(lbl, TB_PAD, Y_REF_LABEL, 13, LIGHTGRAY);

        int max_ref_scroll = n_refs - MAX_VISIBLE_REFS;
        if (max_ref_scroll < 0) max_ref_scroll = 0;
        if (ref_scroll > max_ref_scroll) ref_scroll = max_ref_scroll;
        if (ref_scroll < 0) ref_scroll = 0;

        int ref_visible = n_refs < MAX_VISIBLE_REFS ? n_refs : MAX_VISIBLE_REFS;
        int selected_ref = refimage_selected();
        int renaming_ref = refimage_rename_active() ? refimage_rename_index() : -1;

        for (int i = 0; i < ref_visible; i++) {
            int ri = n_refs - 1 - i - ref_scroll;  // top-down, newest first
            if (ri < 0 || ri >= n_refs) continue;
            RefImage *r = refimage_get(ri);
            float ry = Y_REF_LIST + i * REF_ROW_H;
            Rectangle row = {TB_PAD, ry, TB_INNER, REF_ROW_H - 2};

            Color bg = (ri == selected_ref)
                       ? (Color){50, 70, 120, 255}
                       : (Color){50, 50, 50, 255};
            DrawRectangleRec(row, bg);
            DrawRectangleLinesEx(row, 1, (Color){70, 70, 70, 255});

            // Visibility indicator (click target)
            const char *vis = r->visible ? "O" : "-";
            DrawUI(vis, (int)row.x + 4, (int)ry + 5, 12,
                   r->visible ? GREEN : (Color){80, 80, 80, 255});

            // Lock indicator
            const char *lock = r->locked ? "L" : "-";
            DrawUI(lock, (int)row.x + 20, (int)ry + 5, 12,
                   r->locked ? (Color){220, 160, 50, 255} : (Color){80, 80, 80, 255});

            // Name or inline-rename input
            if (ri == renaming_ref) {
                char *buf = refimage_rename_buffer();
                Rectangle name_r = {(float)row.x + 36, (float)ry + 2, row.width - 40, REF_ROW_H - 6};
                DrawRectangleRec(name_r, (Color){30, 30, 30, 255});
                DrawRectangleLinesEx(name_r, 1, (Color){180, 180, 80, 255});
                DrawUI(buf, (int)name_r.x + 4, (int)ry + 5, 12, WHITE);
            } else {
                const char *nm = r->name[0] ? r->name : "(unnamed)";
                DrawUI(nm, (int)row.x + 36, (int)ry + 5, 12, WHITE);
            }
        }
    }

    rlPopMatrix();
    EndScissorMode();

    // Toolbar scrollbar (right edge, in screen space)
    int tb_content_h = Y_REF_LIST + MAX_VISIBLE_REFS * REF_ROW_H + 10;
    int tb_visible_h = GetScreenHeight();
    draw_scrollbar(TB_W - 6, 0, (float)tb_visible_h,
                   tb_scroll, tb_content_h, tb_visible_h);

    // Layer list scrollbar (inside the layer panel area)
    if (c->layer_count > MAX_VISIBLE_LAYERS) {
        int layer_content = c->layer_count * LAYER_ROW_H;
        int layer_visible = MAX_VISIBLE_LAYERS * LAYER_ROW_H;
        float lsb_x = TB_W - 8;
        float lsb_y = Y_LAYER_LIST - tb_scroll;
        draw_scrollbar(lsb_x, lsb_y, (float)layer_visible,
                       layer_scroll * LAYER_ROW_H, layer_content, layer_visible);
    }
}

ToolbarEvents toolbar_update(ToolState *t, Canvas *c) {
    ToolbarEvents ev = {0};
    Vector2 mouse = GetMousePosition();
    bool ldown    = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    bool lpress   = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

    if (mouse.x > TB_W) return ev;

    // Offset mouse Y for hit testing against scrolled content
    mouse.y += tb_scroll;

    // Determine if mouse is over the layer list area (in scrolled coords)
    Rectangle layer_area = {TB_PAD, Y_LAYER_LIST, TB_INNER,
                            MAX_VISIBLE_LAYERS * LAYER_ROW_H};
    bool over_layers = CheckCollisionPointRec(mouse, layer_area);

    // Scroll: layer panel takes priority when hovered
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        if (over_layers && c->layer_count > MAX_VISIBLE_LAYERS) {
            layer_scroll -= (int)wheel;
            int mls = c->layer_count - MAX_VISIBLE_LAYERS;
            if (layer_scroll < 0) layer_scroll = 0;
            if (layer_scroll > mls) layer_scroll = mls;
        } else {
            tb_scroll -= (int)(wheel * 30);
            if (tb_scroll < 0) tb_scroll = 0;
            int max_scroll = Y_REF_LIST + MAX_VISIBLE_REFS * REF_ROW_H + 10 - GetScreenHeight();
            if (max_scroll < 0) max_scroll = 0;
            if (tb_scroll > max_scroll) tb_scroll = max_scroll;
        }
    }

    // Tool buttons
    Rectangle r_brush  = {TB_PAD,       Y_TOOLS, 60, 30};
    Rectangle r_eraser = {TB_PAD + 65,  Y_TOOLS, 65, 30};
    Rectangle r_line   = {TB_PAD + 135, Y_TOOLS, 60, 30};
    if (lpress && CheckCollisionPointRec(mouse, r_brush))  t->active_tool = TOOL_BRUSH;
    if (lpress && CheckCollisionPointRec(mouse, r_eraser)) t->active_tool = TOOL_ERASER;
    if (lpress && CheckCollisionPointRec(mouse, r_line))   t->active_tool = TOOL_LINE;

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
    if (lpress && CheckCollisionPointRec(mouse, (Rectangle){TB_PAD, Y_BTN_EXPORT, TB_INNER, BTN_H}))
        ev.wants_export = true;
    if (lpress && CheckCollisionPointRec(mouse, (Rectangle){TB_PAD, Y_BTN_CROP, TB_INNER, BTN_H}))
        ev.wants_crop = true;
    if (lpress && CheckCollisionPointRec(mouse, (Rectangle){TB_PAD, Y_BTN_RESIZE, TB_INNER, BTN_H}))
        ev.wants_resize = true;

    // ── Layer panel interaction ──────────────────────────────────────────────

    if (lpress) {
        int visible = c->layer_count < MAX_VISIBLE_LAYERS ? c->layer_count : MAX_VISIBLE_LAYERS;
        for (int i = 0; i < visible; i++) {
            int li = c->layer_count - 1 - i - layer_scroll;
            if (li < 0 || li >= c->layer_count) continue;
            float ry = Y_LAYER_LIST + i * LAYER_ROW_H;
            Rectangle row = {TB_PAD, ry, TB_INNER, LAYER_ROW_H - 2};
            if (CheckCollisionPointRec(mouse, row)) {
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

        // Auto-scroll to keep active layer visible
        int active_row = c->layer_count - 1 - c->active_layer;
        if (active_row < layer_scroll)
            layer_scroll = active_row;
        if (active_row >= layer_scroll + MAX_VISIBLE_LAYERS)
            layer_scroll = active_row - MAX_VISIBLE_LAYERS + 1;
    }

    // ── References panel interaction ─────────────────────────────────────────
    {
        int n_refs = refimage_count();
        int ref_visible = n_refs < MAX_VISIBLE_REFS ? n_refs : MAX_VISIBLE_REFS;
        int renaming = refimage_rename_active() ? refimage_rename_index() : -1;

        // Wheel scrolling over refs list
        Rectangle ref_area = {TB_PAD, Y_REF_LIST, TB_INNER,
                              MAX_VISIBLE_REFS * REF_ROW_H};
        bool over_refs = CheckCollisionPointRec(mouse, ref_area);
        if (wheel != 0.0f && over_refs && n_refs > MAX_VISIBLE_REFS) {
            ref_scroll -= (int)wheel;
            int mrs = n_refs - MAX_VISIBLE_REFS;
            if (ref_scroll < 0) ref_scroll = 0;
            if (ref_scroll > mrs) ref_scroll = mrs;
        }

        for (int i = 0; i < ref_visible; i++) {
            int ri = n_refs - 1 - i - ref_scroll;
            if (ri < 0 || ri >= n_refs) continue;
            float ry = Y_REF_LIST + i * REF_ROW_H;
            Rectangle row = {TB_PAD, ry, TB_INNER, REF_ROW_H - 2};
            Rectangle vis_hit  = {row.x,      row.y, 16, row.height};
            Rectangle lock_hit = {row.x + 16, row.y, 16, row.height};
            Rectangle name_hit = {row.x + 32, row.y, row.width - 32, row.height};

            if (lpress && CheckCollisionPointRec(mouse, vis_hit)) {
                refimage_toggle_visible(ri);
                continue;
            }
            if (lpress && CheckCollisionPointRec(mouse, lock_hit)) {
                refimage_toggle_locked(ri);
                continue;
            }
            if (lpress && CheckCollisionPointRec(mouse, name_hit)) {
                // Double-click begins rename; single click selects
                static double last_click_t = 0;
                static int    last_click_idx = -1;
                double now = GetTime();
                if (last_click_idx == ri && now - last_click_t < 0.4) {
                    refimage_rename_begin(ri);
                    refimage_select(ri);
                    last_click_t = 0;
                    last_click_idx = -1;
                } else {
                    // single click selects
                    refimage_select(ri);
                    last_click_t = now;
                    last_click_idx = ri;
                }
            }
        }

        // Inline-rename text input (only when an image is being renamed)
        if (renaming >= 0) {
            char *buf = refimage_rename_buffer();
            int   len = refimage_rename_buffer_len();

            int ch = GetCharPressed();
            while (ch > 0) {
                if (ch >= 32 && ch < 127 && len < 62) {
                    buf[len++] = (char)ch;
                    buf[len] = '\0';
                    refimage_rename_buffer_set_len(len);
                }
                ch = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && len > 0) {
                len--;
                buf[len] = '\0';
                refimage_rename_buffer_set_len(len);
            }
            if (IsKeyPressed(KEY_ENTER)) {
                refimage_rename_commit();
            }
            if (IsKeyPressed(KEY_ESCAPE)) {
                refimage_rename_cancel();
            }
            // Click anywhere outside the renaming row commits
            if (lpress) {
                int idx = refimage_rename_index();
                if (idx >= 0) {
                    int row_i = n_refs - 1 - idx - ref_scroll;
                    if (row_i >= 0 && row_i < ref_visible) {
                        float ry = Y_REF_LIST + row_i * REF_ROW_H;
                        Rectangle row = {TB_PAD, ry, TB_INNER, REF_ROW_H - 2};
                        if (!CheckCollisionPointRec(mouse, row)) {
                            refimage_rename_commit();
                        }
                    } else {
                        refimage_rename_commit();
                    }
                }
            }
        }
    }

    return ev;
}
