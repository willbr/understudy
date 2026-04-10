#include "canvas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ── Internal helpers ──────────────────────────────────────────────────────────

static void stroke_free_data(Stroke *s) {
    free(s->points);
    s->points   = NULL;
    s->count    = 0;
    s->capacity = 0;
}

// Render one stroke into the current BeginTextureMode target, applying the
// view transform so circles are drawn at display resolution (not at 1:1 doc
// resolution that then gets scaled).  All coordinates are in viewport space.
static void render_stroke_transformed(const Stroke *s,
                                      float vx, float vy, float zoom) {
    if (s->count == 0) return;
    Color color = (s->tool == TOOL_ERASER) ? WHITE : s->color;
    float r = fmaxf(1.0f, (float)s->radius * zoom);

    // First point
    float sx0 = s->points[0].x * zoom + vx;
    float sy0 = s->points[0].y * zoom + vy;
    DrawCircleV((Vector2){sx0, sy0}, r, color);

    // Interpolate between consecutive samples in screen space
    for (int i = 1; i < s->count; i++) {
        float fsx = s->points[i - 1].x * zoom + vx;
        float fsy = s->points[i - 1].y * zoom + vy;
        float tsx = s->points[i].x     * zoom + vx;
        float tsy = s->points[i].y     * zoom + vy;
        float dx  = tsx - fsx, dy = tsy - fsy;
        float dist = sqrtf(dx * dx + dy * dy);
        float step = fmaxf(1.0f, r * 0.5f);
        int   steps = (int)(dist / step) + 1;
        for (int j = 1; j <= steps; j++) {
            float t = (float)j / (float)steps;
            DrawCircleV((Vector2){fsx + dx * t, fsy + dy * t}, r, color);
        }
    }
}

// Full re-render of all committed strokes at the current view transform.
static void redraw_all(Canvas *c) {
    BeginTextureMode(c->rt);
    ClearBackground(WHITE);
    for (int i = 0; i < c->stroke_count; i++)
        render_stroke_transformed(&c->strokes[i], c->view_x, c->view_y, c->zoom);
    EndTextureMode();
}

// Re-render all strokes to the minimap at doc→minimap scale.
// Called after any stroke list change (end, undo, clear, load) — not every frame.
static void update_minimap(Canvas *c) {
    float ms = (float)MINIMAP_SIZE / (float)CANVAS_DOC_W;
    BeginTextureMode(c->minimap_rt);
    ClearBackground(WHITE);
    for (int i = 0; i < c->stroke_count; i++)
        render_stroke_transformed(&c->strokes[i], 0.0f, 0.0f, ms);
    EndTextureMode();
}

static void reset_view(Canvas *c) {
    int pw = c->rt.texture.width;
    int ph = c->rt.texture.height;
    c->zoom   = 1.0f;
    c->view_x = -(CANVAS_DOC_W - pw) / 2.0f;
    c->view_y = -(CANVAS_DOC_H - ph) / 2.0f;
}

// ── Public API ────────────────────────────────────────────────────────────────

void canvas_init(Canvas *c) {
    c->width  = CANVAS_DOC_W;
    c->height = CANVAS_DOC_H;

    // RT is viewport-sized so strokes are always drawn at display resolution
    c->rt = LoadRenderTexture(CANVAS_WIDTH, CANVAS_HEIGHT);
    BeginTextureMode(c->rt);
    ClearBackground(WHITE);
    EndTextureMode();

    c->minimap_rt = LoadRenderTexture(MINIMAP_SIZE, MINIMAP_SIZE);
    BeginTextureMode(c->minimap_rt);
    ClearBackground(WHITE);
    EndTextureMode();

    c->strokes         = NULL;
    c->stroke_count    = 0;
    c->stroke_capacity = 0;
    memset(&c->current, 0, sizeof(c->current));
    c->is_drawing = false;
    c->dirty      = false;

    reset_view(c);
}

void canvas_free(Canvas *c) {
    UnloadRenderTexture(c->minimap_rt);
    UnloadRenderTexture(c->rt);
    for (int i = 0; i < c->stroke_count; i++) stroke_free_data(&c->strokes[i]);
    free(c->strokes);
    stroke_free_data(&c->current);
}

void canvas_begin_stroke(Canvas *c, Color color, int radius, ToolType tool) {
    stroke_free_data(&c->current);
    c->current.color  = color;
    c->current.radius = radius;
    c->current.tool   = tool;
    c->is_drawing     = true;
}

void canvas_add_point(Canvas *c, Vector2 p) {
    if (!c->is_drawing) return;

    Stroke *s = &c->current;
    if (s->count >= s->capacity) {
        int newcap = s->capacity == 0 ? 64 : s->capacity * 2;
        Vector2 *tmp = realloc(s->points, newcap * sizeof(Vector2));
        if (!tmp) return;
        s->points   = tmp;
        s->capacity = newcap;
    }
    s->points[s->count++] = p;

    // Render incremental segment at display resolution using current transform
    Color color = (s->tool == TOOL_ERASER) ? WHITE : s->color;
    float r  = fmaxf(1.0f, (float)s->radius * c->zoom);
    float sx = p.x * c->zoom + c->view_x;
    float sy = p.y * c->zoom + c->view_y;

    BeginTextureMode(c->rt);
    if (s->count == 1) {
        DrawCircleV((Vector2){sx, sy}, r, color);
    } else {
        Vector2 prev = s->points[s->count - 2];
        float fsx = prev.x * c->zoom + c->view_x;
        float fsy = prev.y * c->zoom + c->view_y;
        float dx  = sx - fsx, dy = sy - fsy;
        float dist = sqrtf(dx * dx + dy * dy);
        float step = fmaxf(1.0f, r * 0.5f);
        int   steps = (int)(dist / step) + 1;
        for (int j = 1; j <= steps; j++) {
            float t = (float)j / (float)steps;
            DrawCircleV((Vector2){fsx + dx * t, fsy + dy * t}, r, color);
        }
    }
    EndTextureMode();
    c->dirty = true;
}

void canvas_end_stroke(Canvas *c) {
    if (!c->is_drawing || c->current.count == 0) {
        c->is_drawing = false;
        stroke_free_data(&c->current);
        return;
    }
    if (c->stroke_count >= c->stroke_capacity) {
        int newcap = c->stroke_capacity == 0 ? 16 : c->stroke_capacity * 2;
        Stroke *tmp = realloc(c->strokes, newcap * sizeof(Stroke));
        if (!tmp) { c->is_drawing = false; return; }
        c->strokes         = tmp;
        c->stroke_capacity = newcap;
    }
    c->strokes[c->stroke_count++] = c->current;
    memset(&c->current, 0, sizeof(c->current));
    c->is_drawing = false;
    update_minimap(c);
}

void canvas_undo(Canvas *c) {
    if (c->is_drawing) {
        stroke_free_data(&c->current);
        c->is_drawing = false;
        redraw_all(c);
        update_minimap(c);
        return;
    }
    if (c->stroke_count == 0) return;
    stroke_free_data(&c->strokes[--c->stroke_count]);
    redraw_all(c);
    update_minimap(c);
    c->dirty = (c->stroke_count > 0);
}

void canvas_clear(Canvas *c) {
    for (int i = 0; i < c->stroke_count; i++) stroke_free_data(&c->strokes[i]);
    c->stroke_count = 0;
    stroke_free_data(&c->current);
    c->is_drawing = false;

    BeginTextureMode(c->rt);
    ClearBackground(WHITE);
    EndTextureMode();

    BeginTextureMode(c->minimap_rt);
    ClearBackground(WHITE);
    EndTextureMode();

    c->dirty = false;
    reset_view(c);
}

void canvas_load_strokes(Canvas *c, Stroke *strokes, int count) {
    for (int i = 0; i < c->stroke_count; i++) stroke_free_data(&c->strokes[i]);
    free(c->strokes);
    stroke_free_data(&c->current);
    c->is_drawing = false;

    c->strokes         = strokes;
    c->stroke_count    = count;
    c->stroke_capacity = count;
    c->dirty           = false;

    reset_view(c);
    redraw_all(c);
    update_minimap(c);
}

// Called from main whenever zoom or pan changes — re-renders all strokes at
// the new view transform so they stay crisp at any zoom level.
void canvas_redraw_for_view(Canvas *c) {
    redraw_all(c);
}

void canvas_resize(Canvas *c, int panel_w, int panel_h) {
    UnloadRenderTexture(c->rt);
    c->rt = LoadRenderTexture(panel_w, panel_h);
    redraw_all(c);
}

void canvas_draw_minimap(const Canvas *c, float alpha) {
    if (alpha <= 0.01f) return;

    int panel_w = c->rt.texture.width;
    int panel_h = c->rt.texture.height;

    // Bottom-right of canvas panel
    int mx = CANVAS_X + panel_w - MINIMAP_SIZE - 12;
    int my = panel_h - MINIMAP_SIZE - 12;

    // Dark background with border
    DrawRectangle(mx - 2, my - 2, MINIMAP_SIZE + 4, MINIMAP_SIZE + 4,
                  Fade((Color){15, 15, 15, 210}, alpha));
    DrawRectangleLines(mx - 2, my - 2, MINIMAP_SIZE + 4, MINIMAP_SIZE + 4,
                       Fade((Color){80, 80, 80, 255}, alpha));

    // Thumbnail (Y-flipped per RenderTexture convention)
    Rectangle src  = {0, 0, MINIMAP_SIZE, -(float)MINIMAP_SIZE};
    Rectangle dest = {(float)mx, (float)my, MINIMAP_SIZE, MINIMAP_SIZE};
    DrawTexturePro(c->minimap_rt.texture, src, dest,
                   (Vector2){0, 0}, 0.0f, Fade(WHITE, alpha));

    // Viewport indicator: which portion of the doc is currently visible
    float ms    = (float)MINIMAP_SIZE / (float)CANVAS_DOC_W;
    float vp_x  = (-c->view_x / c->zoom) * ms;
    float vp_y  = (-c->view_y / c->zoom) * ms;
    float vp_w  = ((float)panel_w / c->zoom) * ms;
    float vp_h  = ((float)panel_h / c->zoom) * ms;

    Rectangle vp_rect = {mx + vp_x, my + vp_y, vp_w, vp_h};
    BeginScissorMode(mx, my, MINIMAP_SIZE, MINIMAP_SIZE);
    DrawRectangleLinesEx(vp_rect, 1.5f, Fade((Color){80, 160, 255, 255}, alpha));
    EndScissorMode();
}

void canvas_draw(const Canvas *c) {
    int pw = c->rt.texture.width;
    int ph = c->rt.texture.height;

    // The RT is viewport-sized with the transform already baked in.
    // Draw it at the panel origin (Y-flipped per RenderTexture convention).
    Rectangle src  = {0, 0, (float)pw, -(float)ph};
    Rectangle dest = {CANVAS_X, CANVAS_Y, (float)pw, (float)ph};
    DrawTexturePro(c->rt.texture, src, dest, (Vector2){0, 0}, 0.0f, WHITE);

    // Document boundary — only visible when panned near the edge
    float bx = CANVAS_X + c->view_x;
    float by = CANVAS_Y + c->view_y;
    float bw = (float)c->width  * c->zoom;
    float bh = (float)c->height * c->zoom;
    BeginScissorMode(CANVAS_X, CANVAS_Y, pw, ph);
    DrawRectangleLinesEx((Rectangle){bx, by, bw, bh}, 1.0f,
                         (Color){90, 90, 90, 180});
    EndScissorMode();
}
