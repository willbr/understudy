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

// Render one stroke into c->rt.  Call between BeginTextureMode/EndTextureMode.
static void render_stroke(const Stroke *s) {
    if (s->count == 0) return;
    Color color = (s->tool == TOOL_ERASER) ? WHITE : s->color;

    DrawCircle((int)s->points[0].x, (int)s->points[0].y, (float)s->radius, color);

    for (int i = 1; i < s->count; i++) {
        Vector2 from = s->points[i - 1];
        Vector2 to   = s->points[i];
        float dx     = to.x - from.x;
        float dy     = to.y - from.y;
        float dist   = sqrtf(dx * dx + dy * dy);
        float step   = fmaxf(1.0f, (float)s->radius * 0.5f);
        int   steps  = (int)(dist / step) + 1;
        for (int j = 1; j <= steps; j++) {
            float t = (float)j / (float)steps;
            DrawCircle((int)(from.x + dx * t), (int)(from.y + dy * t),
                       (float)s->radius, color);
        }
    }
}

static void redraw_all(Canvas *c) {
    BeginTextureMode(c->rt);
    ClearBackground(WHITE);
    for (int i = 0; i < c->stroke_count; i++) render_stroke(&c->strokes[i]);
    EndTextureMode();
}

// ── Public API ────────────────────────────────────────────────────────────────

void canvas_init(Canvas *c) {
    c->width  = CANVAS_DOC_W;
    c->height = CANVAS_DOC_H;
    c->rt     = LoadRenderTexture(c->width, c->height);

    BeginTextureMode(c->rt);
    ClearBackground(WHITE);
    EndTextureMode();

    c->strokes          = NULL;
    c->stroke_count     = 0;
    c->stroke_capacity  = 0;
    memset(&c->current, 0, sizeof(c->current));
    c->is_drawing = false;
    c->dirty      = false;

    // Center the document in the display panel on startup
    c->view_x = -(CANVAS_DOC_W - CANVAS_WIDTH)  / 2;
    c->view_y = -(CANVAS_DOC_H - CANVAS_HEIGHT) / 2;
}

void canvas_free(Canvas *c) {
    UnloadRenderTexture(c->rt);
    for (int i = 0; i < c->stroke_count; i++) stroke_free_data(&c->strokes[i]);
    free(c->strokes);
    stroke_free_data(&c->current);
}

void canvas_begin_stroke(Canvas *c, Color color, int radius, ToolType tool) {
    stroke_free_data(&c->current);
    c->current.color   = color;
    c->current.radius  = radius;
    c->current.tool    = tool;
    c->is_drawing      = true;
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

    // Render the new segment incrementally (avoids replaying all strokes)
    Color color = (s->tool == TOOL_ERASER) ? WHITE : s->color;
    BeginTextureMode(c->rt);
    if (s->count == 1) {
        DrawCircle((int)p.x, (int)p.y, (float)s->radius, color);
    } else {
        Vector2 from = s->points[s->count - 2];
        float dx = p.x - from.x, dy = p.y - from.y;
        float dist = sqrtf(dx * dx + dy * dy);
        float step = fmaxf(1.0f, (float)s->radius * 0.5f);
        int steps  = (int)(dist / step) + 1;
        for (int j = 1; j <= steps; j++) {
            float t = (float)j / (float)steps;
            DrawCircle((int)(from.x + dx * t), (int)(from.y + dy * t),
                       (float)s->radius, color);
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

    // Grow the committed array and transfer ownership from current
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
}

void canvas_undo(Canvas *c) {
    if (c->is_drawing) {
        // Cancel in-progress stroke
        stroke_free_data(&c->current);
        c->is_drawing = false;
        redraw_all(c);
        return;
    }
    if (c->stroke_count == 0) return;
    stroke_free_data(&c->strokes[--c->stroke_count]);
    redraw_all(c);
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

    c->dirty  = false;
    c->view_x = -(CANVAS_DOC_W - CANVAS_WIDTH)  / 2;
    c->view_y = -(CANVAS_DOC_H - CANVAS_HEIGHT) / 2;
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
    c->view_x          = -(CANVAS_DOC_W - CANVAS_WIDTH)  / 2;
    c->view_y          = -(CANVAS_DOC_H - CANVAS_HEIGHT) / 2;

    redraw_all(c);
}

void canvas_draw(const Canvas *c) {
    float dx = (float)(CANVAS_X + c->view_x);
    float dy = (float)(CANVAS_Y + c->view_y);

    // Clip all canvas drawing to the panel area so it never bleeds into the toolbar
    BeginScissorMode(CANVAS_X, CANVAS_Y, CANVAS_WIDTH, CANVAS_HEIGHT);

    // Drop shadow (only visible near the document edge when panned)
    DrawRectangle((int)dx + 4, (int)dy + 4, c->width, c->height,
                  (Color){0, 0, 0, 70});

    // RenderTexture2D is stored flipped — negate source height to correct it
    Rectangle src  = {0, 0, (float)c->width, -(float)c->height};
    Rectangle dest = {dx, dy, (float)c->width, (float)c->height};
    DrawTexturePro(c->rt.texture, src, dest, (Vector2){0, 0}, 0.0f, WHITE);

    // Thin border so the document edge is identifiable when panned near it
    DrawRectangleLinesEx(dest, 1, (Color){90, 90, 90, 255});

    EndScissorMode();
}
