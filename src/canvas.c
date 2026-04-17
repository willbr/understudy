#include "canvas.h"
#include "rlgl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ── Paper texture ────────────────────────────────────────────────────────────

#define PAPER_TILE 256

static unsigned int paper_hash(int x, int y) {
    unsigned int h = (unsigned int)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177;
    return h ^ (h >> 16);
}

static Texture2D gen_paper_texture(void) {
    Image img = GenImageColor(PAPER_TILE, PAPER_TILE, WHITE);
    Color *px = (Color *)img.data;
    for (int y = 0; y < PAPER_TILE; y++) {
        for (int x = 0; x < PAPER_TILE; x++) {
            int n1 = (int)(paper_hash(x, y) % 12) - 6;
            int n2 = (int)(paper_hash(x / 4, y / 4 + 997) % 6) - 3;
            int v  = 244 + n1 + n2;
            if (v < 228) v = 228;
            if (v > 255) v = 255;
            px[y * PAPER_TILE + x] = (Color){
                (unsigned char)v,
                (unsigned char)v,
                (unsigned char)(v > 3 ? v - 3 : 0),
                255
            };
        }
    }
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    SetTextureWrap(tex, TEXTURE_WRAP_REPEAT);
    return tex;
}

// Bitmap-based paper for minimap (shader not needed at tiny scale)
static void draw_paper_bitmap(const Canvas *c, float vx, float vy, float zoom) {
    Rectangle src  = {0, 0, (float)c->width, (float)c->height};
    Rectangle dest = {vx, vy, (float)c->width * zoom,
                               (float)c->height * zoom};
    DrawTexturePro(c->paper_tex, src, dest, (Vector2){0, 0}, 0.0f, WHITE);
}

// ── Internal helpers ──────────────────────────────────────────────────────────

static void stroke_free_data(Stroke *s) {
    free(s->points);
    s->points   = NULL;
    s->count    = 0;
    s->capacity = 0;
}

static void layer_free(Layer *l) {
    for (int i = 0; i < l->stroke_count; i++)
        stroke_free_data(&l->strokes[i]);
    free(l->strokes);
    l->strokes         = NULL;
    l->stroke_count    = 0;
    l->stroke_capacity = 0;
}

static void layer_init(Layer *l, const char *name) {
    memset(l, 0, sizeof(*l));
    snprintf(l->name, LAYER_NAME_LEN, "%s", name);
    l->visible = true;
}

static Layer *active_layer(Canvas *c) {
    return &c->layers[c->active_layer];
}

// Catmull-Rom spline interpolation between 4 points
static Vector2 catmull_rom(Vector2 p0, Vector2 p1, Vector2 p2, Vector2 p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return (Vector2){
        0.5f * (2.0f*p1.x + (-p0.x+p2.x)*t + (2.0f*p0.x-5.0f*p1.x+4.0f*p2.x-p3.x)*t2 + (-p0.x+3.0f*p1.x-3.0f*p2.x+p3.x)*t3),
        0.5f * (2.0f*p1.y + (-p0.y+p2.y)*t + (2.0f*p0.y-5.0f*p1.y+4.0f*p2.y-p3.y)*t2 + (-p0.y+3.0f*p1.y-3.0f*p2.y+p3.y)*t3)
    };
}

void render_stroke_transformed(const Stroke *s,
                               float vx, float vy, float zoom) {
    if (s->count == 0) return;

    // Eraser: overwrite with transparent to punch holes (reveals paper)
    bool eraser = (s->tool == TOOL_ERASER);
    Color color = eraser ? BLANK : s->color;
    if (eraser) {
        rlDrawRenderBatchActive();
        rlSetBlendFactors(0x1, 0x0, 0x8006); // GL_ONE, GL_ZERO, GL_FUNC_ADD
        rlSetBlendMode(RL_BLEND_CUSTOM);
    }

    float base_r = fmaxf(1.0f, (float)s->radius * zoom);

    // Single point — small dot
    if (s->count == 1) {
        float sx0 = s->points[0].x * zoom + vx;
        float sy0 = s->points[0].y * zoom + vy;
        DrawCircleV((Vector2){sx0, sy0}, base_r * 0.5f, color);
        goto done;
    }

    // Line tool: straight line between two points, uniform width
    if (s->tool == TOOL_LINE && s->count == 2) {
        float fsx = s->points[0].x * zoom + vx;
        float fsy = s->points[0].y * zoom + vy;
        float tsx = s->points[1].x * zoom + vx;
        float tsy = s->points[1].y * zoom + vy;
        float dx = tsx - fsx, dy = tsy - fsy;
        float dist = sqrtf(dx*dx + dy*dy);
        float step = fmaxf(0.5f, base_r * 0.25f);
        int steps = (int)(dist / step) + 1;
        for (int j = 0; j <= steps; j++) {
            float t = (float)j / (float)steps;
            DrawCircleV((Vector2){fsx + dx*t, fsy + dy*t}, base_r, color);
        }
        goto done;
    }

    // Two points — just a line segment
    if (s->count == 2) {
        float fsx = s->points[0].x * zoom + vx;
        float fsy = s->points[0].y * zoom + vy;
        float tsx = s->points[1].x * zoom + vx;
        float tsy = s->points[1].y * zoom + vy;
        float dx = tsx - fsx, dy = tsy - fsy;
        float dist = sqrtf(dx*dx + dy*dy);
        float r = base_r * 0.7f;
        float step = fmaxf(0.5f, r * 0.25f);
        int steps = (int)(dist / step) + 1;
        for (int j = 0; j <= steps; j++) {
            float t = (float)j / (float)steps;
            DrawCircleV((Vector2){fsx + dx*t, fsy + dy*t}, r, color);
        }
        goto done;
    }

    // 3+ points: Catmull-Rom spline for smooth curves
    for (int i = 0; i < s->count - 1; i++) {
        // Get 4 control points (clamp at boundaries)
        int i0 = (i > 0) ? i - 1 : 0;
        int i1 = i;
        int i2 = i + 1;
        int i3 = (i + 2 < s->count) ? i + 2 : s->count - 1;

        Vector2 p0 = {s->points[i0].x * zoom + vx, s->points[i0].y * zoom + vy};
        Vector2 p1 = {s->points[i1].x * zoom + vx, s->points[i1].y * zoom + vy};
        Vector2 p2 = {s->points[i2].x * zoom + vx, s->points[i2].y * zoom + vy};
        Vector2 p3 = {s->points[i3].x * zoom + vx, s->points[i3].y * zoom + vy};

        // Segment distance (straight) for step count
        float dx = p2.x - p1.x, dy = p2.y - p1.y;
        float dist = sqrtf(dx*dx + dy*dy);

        // Speed-based radius
        float speed = dist / fmaxf(base_r, 1.0f);
        float speed_factor = 1.0f - 0.3f * fminf(speed / 3.0f, 1.0f);
        float r = base_r * speed_factor;

        // Taper at start and end
        float start_taper = fminf((float)(i + 1) / 3.0f, 1.0f);
        float end_taper   = fminf((float)(s->count - 1 - i) / 3.0f, 1.0f);
        float taper = fminf(start_taper, end_taper);
        r *= (0.4f + 0.6f * taper);

        // Interpolate along spline
        float step = fmaxf(0.5f, r * 0.25f);
        int steps = (int)(dist / step) + 1;
        if (steps < 2) steps = 2;
        for (int j = 0; j <= steps; j++) {
            float t = (float)j / (float)steps;
            Vector2 pt = catmull_rom(p0, p1, p2, p3, t);
            DrawCircleV(pt, r, color);
        }
    }

done:
    if (eraser) {
        rlDrawRenderBatchActive();
        rlSetBlendMode(RL_BLEND_ALPHA);
    }
}

static void render_paper(Canvas *c) {
    int pw = c->paper_rt.texture.width;
    int ph = c->paper_rt.texture.height;

    BeginTextureMode(c->paper_rt);
    ClearBackground(BLANK);

    float docSize[2] = {(float)c->width, (float)c->height};
    float viewOff[2] = {c->view_x, c->view_y};
    float res[2]     = {(float)pw, (float)ph};
    SetShaderValue(c->paper_shader,
                   GetShaderLocation(c->paper_shader, "docSize"),
                   docSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(c->paper_shader,
                   GetShaderLocation(c->paper_shader, "viewOffset"),
                   viewOff, SHADER_UNIFORM_VEC2);
    SetShaderValue(c->paper_shader,
                   GetShaderLocation(c->paper_shader, "zoom"),
                   &c->zoom, SHADER_UNIFORM_FLOAT);
    SetShaderValue(c->paper_shader,
                   GetShaderLocation(c->paper_shader, "resolution"),
                   res, SHADER_UNIFORM_VEC2);

    BeginShaderMode(c->paper_shader);
    float sx = c->view_x;
    float sy = c->view_y;
    float sw = (float)c->width  * c->zoom;
    float sh = (float)c->height * c->zoom;
    DrawRectangle((int)sx, (int)sy, (int)sw, (int)sh, WHITE);
    EndShaderMode();

    EndTextureMode();
}

// Full re-render of all visible layers at the current view transform.
// Composite strokes_rt into rt using the ink shader
void composite_ink(Canvas *c) {
    int pw = c->rt.texture.width;
    int ph = c->rt.texture.height;

    BeginTextureMode(c->rt);
    ClearBackground(BLANK);

    float docSize[2] = {(float)c->width, (float)c->height};
    float viewOff[2] = {c->view_x, c->view_y};
    float res[2]     = {(float)pw, (float)ph};
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "docSize"),
                   docSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "viewOffset"),
                   viewOff, SHADER_UNIFORM_VEC2);
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "zoom"),
                   &c->zoom, SHADER_UNIFORM_FLOAT);
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "resolution"),
                   res, SHADER_UNIFORM_VEC2);

    BeginShaderMode(c->ink_shader);
    Rectangle src  = {0, 0, (float)pw, -(float)ph};
    Rectangle dest = {0, 0, (float)pw, (float)ph};
    DrawTexturePro(c->strokes_rt.texture, src, dest,
                   (Vector2){0, 0}, 0.0f, WHITE);
    EndShaderMode();
    EndTextureMode();
}

static void redraw_committed(Canvas *c) {
    int pw = c->committed_rt.texture.width;
    int ph = c->committed_rt.texture.height;

    // Clear the final composite
    BeginTextureMode(c->committed_rt);
    ClearBackground(BLANK);
    EndTextureMode();

    // Render each layer separately so erasers only affect their own layer
    for (int li = 0; li < c->layer_count; li++) {
        if (!c->layers[li].visible) continue;
        Layer *l = &c->layers[li];
        if (l->stroke_count == 0) continue;

        // Render this layer's strokes to strokes_rt (used as scratch)
        BeginTextureMode(c->strokes_rt);
        ClearBackground(BLANK);
        for (int si = 0; si < l->stroke_count; si++)
            render_stroke_transformed(&l->strokes[si],
                                      c->view_x, c->view_y, c->zoom);
        EndTextureMode();

        // Composite this layer onto committed_rt with alpha blending
        // Erased holes (alpha=0) let lower layers show through
        BeginTextureMode(c->committed_rt);
        Rectangle src  = {0, 0, (float)pw, -(float)ph};
        Rectangle dest = {0, 0, (float)pw, (float)ph};
        DrawTexturePro(c->strokes_rt.texture, src, dest,
                       (Vector2){0, 0}, 0.0f, WHITE);
        EndTextureMode();
    }
}

static void redraw_all(Canvas *c) {
    int pw = c->strokes_rt.texture.width;
    int ph = c->strokes_rt.texture.height;

    // Rebuild committed strokes cache (per-layer compositing)
    redraw_committed(c);

    // Copy committed_rt to strokes_rt for the ink shader
    BeginTextureMode(c->strokes_rt);
    ClearBackground(BLANK);
    Rectangle src  = {0, 0, (float)pw, -(float)ph};
    Rectangle dest = {0, 0, (float)pw, (float)ph};
    DrawTexturePro(c->committed_rt.texture, src, dest,
                   (Vector2){0, 0}, 0.0f, WHITE);
    EndTextureMode();

    composite_ink(c);
}

static void update_minimap(Canvas *c) {
    int max_dim = c->width > c->height ? c->width : c->height;
    float ms = (float)MINIMAP_SIZE / (float)max_dim;
    BeginTextureMode(c->minimap_rt);
    ClearBackground((Color){45, 45, 45, 255});
    draw_paper_bitmap(c, 0.0f, 0.0f, ms);
    for (int li = 0; li < c->layer_count; li++) {
        if (!c->layers[li].visible) continue;
        Layer *l = &c->layers[li];
        for (int si = 0; si < l->stroke_count; si++)
            render_stroke_transformed(&l->strokes[si], 0.0f, 0.0f, ms);
    }
    EndTextureMode();
}

static void reset_view(Canvas *c) {
    int pw = c->rt.texture.width;
    int ph = c->rt.texture.height;
    c->zoom   = 1.0f;
    c->view_x = -(c->width - pw) / 2.0f;
    c->view_y = -(c->height - ph) / 2.0f;
}

// Total stroke count across all layers (for status display)
static int total_strokes(const Canvas *c) {
    int n = 0;
    for (int i = 0; i < c->layer_count; i++)
        n += c->layers[i].stroke_count;
    return n;
}

// ── Public API ────────────────────────────────────────────────────────────────

void canvas_init(Canvas *c) {
    c->width  = CANVAS_DOC_W;
    c->height = CANVAS_DOC_H;

    c->paper_tex = gen_paper_texture();
    c->ink_shader   = LoadShader(NULL, "src/ink.fs");
    c->paper_shader = LoadShader(0, "src/paper.fs");

    c->rt           = LoadRenderTexture(CANVAS_WIDTH, CANVAS_HEIGHT);
    c->strokes_rt   = LoadRenderTexture(CANVAS_WIDTH, CANVAS_HEIGHT);
    c->committed_rt = LoadRenderTexture(CANVAS_WIDTH, CANVAS_HEIGHT);
    c->paper_rt     = LoadRenderTexture(CANVAS_WIDTH, CANVAS_HEIGHT);
    c->minimap_rt   = LoadRenderTexture(MINIMAP_SIZE, MINIMAP_SIZE);

    memset(c->layers, 0, sizeof(c->layers));
    c->layer_count  = 1;
    c->active_layer = 0;
    layer_init(&c->layers[0], "Background");

    memset(&c->current, 0, sizeof(c->current));
    c->is_drawing = false;
    c->dirty      = false;

    reset_view(c);
    redraw_all(c);
    render_paper(c);
    update_minimap(c);
}

void canvas_free(Canvas *c) {
    UnloadShader(c->ink_shader);
    UnloadShader(c->paper_shader);
    UnloadTexture(c->paper_tex);
    UnloadRenderTexture(c->minimap_rt);
    UnloadRenderTexture(c->paper_rt);
    UnloadRenderTexture(c->committed_rt);
    UnloadRenderTexture(c->strokes_rt);
    UnloadRenderTexture(c->rt);
    for (int i = 0; i < c->layer_count; i++)
        layer_free(&c->layers[i]);
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

    int pw = c->strokes_rt.texture.width;
    int ph = c->strokes_rt.texture.height;
    Rectangle src  = {0, 0, (float)pw, -(float)ph};
    Rectangle dest = {0, 0, (float)pw, (float)ph};

    if (s->tool == TOOL_ERASER) {
        // Eraser needs per-layer compositing so it only erases the active layer
        // Render all layers, inserting the current eraser into the active layer
        BeginTextureMode(c->committed_rt);
        ClearBackground(BLANK);
        EndTextureMode();

        for (int li = 0; li < c->layer_count; li++) {
            if (!c->layers[li].visible) continue;
            Layer *l = &c->layers[li];

            BeginTextureMode(c->strokes_rt);
            ClearBackground(BLANK);
            for (int si = 0; si < l->stroke_count; si++)
                render_stroke_transformed(&l->strokes[si],
                                          c->view_x, c->view_y, c->zoom);
            // Add current eraser stroke to active layer
            if (li == c->active_layer)
                render_stroke_transformed(s, c->view_x, c->view_y, c->zoom);
            EndTextureMode();

            BeginTextureMode(c->committed_rt);
            DrawTexturePro(c->strokes_rt.texture, src, dest,
                           (Vector2){0, 0}, 0.0f, WHITE);
            EndTextureMode();
        }

        // Copy result to strokes_rt for ink shader
        BeginTextureMode(c->strokes_rt);
        ClearBackground(BLANK);
        DrawTexturePro(c->committed_rt.texture, src, dest,
                       (Vector2){0, 0}, 0.0f, WHITE);
        EndTextureMode();
    } else {
        // Brush/Line: fast path — copy committed cache, draw current stroke on top
        BeginTextureMode(c->strokes_rt);
        ClearBackground(BLANK);
        DrawTexturePro(c->committed_rt.texture, src, dest,
                       (Vector2){0, 0}, 0.0f, WHITE);
        render_stroke_transformed(s, c->view_x, c->view_y, c->zoom);
        EndTextureMode();
    }

    composite_ink(c);
    c->dirty = true;
}

void canvas_end_stroke(Canvas *c) {
    if (!c->is_drawing || c->current.count == 0) {
        c->is_drawing = false;
        stroke_free_data(&c->current);
        return;
    }
    Layer *l = active_layer(c);
    if (l->stroke_count >= l->stroke_capacity) {
        int newcap = l->stroke_capacity == 0 ? 16 : l->stroke_capacity * 2;
        Stroke *tmp = realloc(l->strokes, newcap * sizeof(Stroke));
        if (!tmp) { c->is_drawing = false; return; }
        l->strokes         = tmp;
        l->stroke_capacity = newcap;
    }
    l->strokes[l->stroke_count++] = c->current;
    memset(&c->current, 0, sizeof(c->current));
    c->is_drawing = false;
    redraw_all(c);      // ensure correct layer compositing order
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
    Layer *l = active_layer(c);
    if (l->stroke_count == 0) return;
    stroke_free_data(&l->strokes[--l->stroke_count]);
    redraw_all(c);
    update_minimap(c);
    c->dirty = (total_strokes(c) > 0);
}

void canvas_clear(Canvas *c) {
    for (int i = 0; i < c->layer_count; i++)
        layer_free(&c->layers[i]);
    c->layer_count  = 1;
    c->active_layer = 0;
    layer_init(&c->layers[0], "Background");

    stroke_free_data(&c->current);
    c->is_drawing = false;
    c->dirty      = false;

    reset_view(c);
    redraw_all(c);
    update_minimap(c);
}

void canvas_load_layers(Canvas *c, Layer *layers, int layer_count) {
    for (int i = 0; i < c->layer_count; i++)
        layer_free(&c->layers[i]);
    stroke_free_data(&c->current);
    c->is_drawing = false;

    // Copy layers into the fixed array
    int count = layer_count < MAX_LAYERS ? layer_count : MAX_LAYERS;
    for (int i = 0; i < count; i++)
        c->layers[i] = layers[i];
    c->layer_count  = count;
    c->active_layer = 0;
    c->dirty        = false;

    free(layers);  // free the container (layer contents now owned by c->layers[])

    reset_view(c);
    redraw_all(c);
    update_minimap(c);
}

bool canvas_export_png(Canvas *c, const char *path, int scale) {
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    int ew = c->width * scale, eh = c->height * scale;

    // Render at 2x target resolution for anti-aliasing (supersampling)
    int ssw = ew * 2, ssh = eh * 2;
    float ss_zoom = (float)(scale * 2);

    RenderTexture2D strokes = LoadRenderTexture(ssw, ssh);
    BeginTextureMode(strokes);
    ClearBackground(BLANK);
    for (int li = 0; li < c->layer_count; li++) {
        if (!c->layers[li].visible) continue;
        Layer *l = &c->layers[li];
        for (int si = 0; si < l->stroke_count; si++)
            render_stroke_transformed(&l->strokes[si], 0.0f, 0.0f, ss_zoom);
    }
    EndTextureMode();

    // Composite with ink shader at supersample resolution
    RenderTexture2D ss_rt = LoadRenderTexture(ssw, ssh);
    BeginTextureMode(ss_rt);
    ClearBackground(WHITE);
    float docSize[2] = {(float)c->width, (float)c->height};
    float viewOff[2] = {0.0f, 0.0f};
    float res[2]     = {(float)ssw, (float)ssh};
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "docSize"), docSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "viewOffset"), viewOff, SHADER_UNIFORM_VEC2);
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "zoom"), &ss_zoom, SHADER_UNIFORM_FLOAT);
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "resolution"), res, SHADER_UNIFORM_VEC2);
    BeginShaderMode(c->ink_shader);
    Rectangle ss_src  = {0, 0, (float)ssw, -(float)ssh};
    Rectangle ss_dest = {0, 0, (float)ssw, (float)ssh};
    DrawTexturePro(strokes.texture, ss_src, ss_dest, (Vector2){0, 0}, 0.0f, WHITE);
    EndShaderMode();
    EndTextureMode();
    UnloadRenderTexture(strokes);

    // Downsample 2x with bilinear filtering to target resolution
    SetTextureFilter(ss_rt.texture, TEXTURE_FILTER_BILINEAR);
    RenderTexture2D export_rt = LoadRenderTexture(ew, eh);
    BeginTextureMode(export_rt);
    Rectangle down_src  = {0, 0, (float)ssw, -(float)ssh};
    Rectangle down_dest = {0, 0, (float)ew, (float)eh};
    DrawTexturePro(ss_rt.texture, down_src, down_dest, (Vector2){0, 0}, 0.0f, WHITE);
    EndTextureMode();
    UnloadRenderTexture(ss_rt);

    // Grab image (Y-flip needed for RenderTexture)
    Image img = LoadImageFromTexture(export_rt.texture);
    ImageFlipVertical(&img);
    bool ok = ExportImage(img, path);
    UnloadImage(img);
    UnloadRenderTexture(export_rt);
    return ok;
}

void canvas_crop(Canvas *c, int x, int y, int w, int h) {
    if (c->is_drawing) canvas_end_stroke(c);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > c->width) w = c->width - x;
    if (y + h > c->height) h = c->height - y;

    // Offset all stroke points by the crop origin, then clip to new bounds
    for (int li = 0; li < c->layer_count; li++) {
        Layer *l = &c->layers[li];
        int write = 0;
        for (int si = 0; si < l->stroke_count; si++) {
            Stroke *s = &l->strokes[si];
            // Offset points
            for (int pi = 0; pi < s->count; pi++) {
                s->points[pi].x -= (float)x;
                s->points[pi].y -= (float)y;
            }
            // Clip: keep only points inside new bounds
            int pw = 0;
            for (int pi = 0; pi < s->count; pi++) {
                if (s->points[pi].x >= 0 && s->points[pi].x < w &&
                    s->points[pi].y >= 0 && s->points[pi].y < h) {
                    s->points[pw++] = s->points[pi];
                }
            }
            s->count = pw;
            if (pw == 0) {
                stroke_free_data(s);
            } else {
                l->strokes[write++] = *s;
            }
        }
        l->stroke_count = write;
    }
    c->width  = w;
    c->height = h;
    c->dirty  = true;
    reset_view(c);
    redraw_all(c);
    update_minimap(c);
}

void canvas_resize_doc(Canvas *c, int new_w, int new_h) {
    if (c->is_drawing) canvas_end_stroke(c);
    if (new_w < 64) new_w = 64;
    if (new_h < 64) new_h = 64;
    if (new_w > 16384) new_w = 16384;
    if (new_h > 16384) new_h = 16384;

    float sx = (float)new_w / (float)c->width;
    float sy = (float)new_h / (float)c->height;
    float sr = (sx + sy) * 0.5f;  // average scale for brush radius

    for (int li = 0; li < c->layer_count; li++) {
        Layer *l = &c->layers[li];
        for (int si = 0; si < l->stroke_count; si++) {
            Stroke *s = &l->strokes[si];
            for (int pi = 0; pi < s->count; pi++) {
                s->points[pi].x *= sx;
                s->points[pi].y *= sy;
            }
            s->radius = (int)(s->radius * sr + 0.5f);
            if (s->radius < 1) s->radius = 1;
        }
    }
    c->width  = new_w;
    c->height = new_h;
    c->dirty  = true;
    reset_view(c);
    redraw_all(c);
    update_minimap(c);
}

void canvas_redraw_for_view(Canvas *c) {
    redraw_all(c);
    render_paper(c);
}

void canvas_resize(Canvas *c, int panel_w, int panel_h) {
    UnloadRenderTexture(c->rt);
    UnloadRenderTexture(c->strokes_rt);
    UnloadRenderTexture(c->committed_rt);
    UnloadRenderTexture(c->paper_rt);
    c->rt           = LoadRenderTexture(panel_w, panel_h);
    c->strokes_rt   = LoadRenderTexture(panel_w, panel_h);
    c->committed_rt = LoadRenderTexture(panel_w, panel_h);
    c->paper_rt     = LoadRenderTexture(panel_w, panel_h);
    redraw_all(c);
    render_paper(c);
}

void canvas_draw_minimap(const Canvas *c, float alpha, int x_offset) {
    if (alpha <= 0.01f) return;

    int panel_w = c->rt.texture.width;
    int panel_h = c->rt.texture.height;

    int mx = x_offset + panel_w - MINIMAP_SIZE - 12;
    int my = panel_h - MINIMAP_SIZE - 12;

    DrawRectangle(mx - 2, my - 2, MINIMAP_SIZE + 4, MINIMAP_SIZE + 4,
                  Fade((Color){15, 15, 15, 210}, alpha));
    DrawRectangleLines(mx - 2, my - 2, MINIMAP_SIZE + 4, MINIMAP_SIZE + 4,
                       Fade((Color){80, 80, 80, 255}, alpha));

    Rectangle src  = {0, 0, MINIMAP_SIZE, -(float)MINIMAP_SIZE};
    Rectangle dest = {(float)mx, (float)my, MINIMAP_SIZE, MINIMAP_SIZE};
    DrawTexturePro(c->minimap_rt.texture, src, dest,
                   (Vector2){0, 0}, 0.0f, Fade(WHITE, alpha));

    int max_dim = c->width > c->height ? c->width : c->height;
    float ms    = (float)MINIMAP_SIZE / (float)max_dim;
    float vp_x  = (-c->view_x / c->zoom) * ms;
    float vp_y  = (-c->view_y / c->zoom) * ms;
    float vp_w  = ((float)panel_w / c->zoom) * ms;
    float vp_h  = ((float)panel_h / c->zoom) * ms;

    Rectangle vp_rect = {mx + vp_x, my + vp_y, vp_w, vp_h};
    BeginScissorMode(mx, my, MINIMAP_SIZE, MINIMAP_SIZE);
    DrawRectangleLinesEx(vp_rect, 1.5f, Fade((Color){80, 160, 255, 255}, alpha));
    EndScissorMode();
}

void canvas_draw_dark_bg(const Canvas *c, int x_offset) {
    int pw = c->rt.texture.width;
    int ph = c->rt.texture.height;
    DrawRectangle(x_offset, CANVAS_Y, pw, ph, (Color){45, 45, 45, 255});
}

void canvas_draw_paper(const Canvas *c, int x_offset) {
    int pw = c->paper_rt.texture.width;
    int ph = c->paper_rt.texture.height;
    Rectangle src  = {0, 0, (float)pw, -(float)ph};
    Rectangle dest = {(float)x_offset, CANVAS_Y, (float)pw, (float)ph};
    DrawTexturePro(c->paper_rt.texture, src, dest, (Vector2){0, 0}, 0.0f, WHITE);
}

void canvas_draw_strokes(const Canvas *c, int x_offset) {
    int pw = c->rt.texture.width;
    int ph = c->rt.texture.height;
    Rectangle src  = {0, 0, (float)pw, -(float)ph};
    Rectangle dest = {(float)x_offset, CANVAS_Y, (float)pw, (float)ph};
    DrawTexturePro(c->rt.texture, src, dest, (Vector2){0, 0}, 0.0f, WHITE);
}

void canvas_draw_border(const Canvas *c, int x_offset) {
    int pw = c->rt.texture.width;
    int ph = c->rt.texture.height;
    float bx = (float)x_offset + c->view_x;
    float by = CANVAS_Y + c->view_y;
    float bw = (float)c->width  * c->zoom;
    float bh = (float)c->height * c->zoom;
    BeginScissorMode(x_offset, CANVAS_Y, pw, ph);
    DrawRectangleLinesEx((Rectangle){bx, by, bw, bh}, 1.0f,
                         (Color){90, 90, 90, 180});
    EndScissorMode();
}

// ── Layer management ─────────────────────────────────────────────────────────

int canvas_add_layer(Canvas *c) {
    if (c->layer_count >= MAX_LAYERS) return -1;

    // Insert above active layer
    int idx = c->active_layer + 1;
    memmove(&c->layers[idx + 1], &c->layers[idx],
            (c->layer_count - idx) * sizeof(Layer));
    c->layer_count++;

    char name[LAYER_NAME_LEN];
    snprintf(name, sizeof(name), "Layer %d", c->layer_count);
    layer_init(&c->layers[idx], name);

    c->active_layer = idx;
    c->dirty = true;
    return idx;
}

void canvas_delete_layer(Canvas *c, int idx) {
    if (c->layer_count <= 1) return;
    if (idx < 0 || idx >= c->layer_count) return;

    layer_free(&c->layers[idx]);
    memmove(&c->layers[idx], &c->layers[idx + 1],
            (c->layer_count - idx - 1) * sizeof(Layer));
    c->layer_count--;

    if (c->active_layer >= c->layer_count)
        c->active_layer = c->layer_count - 1;
    if (c->active_layer > idx && c->active_layer > 0)
        c->active_layer--;

    c->dirty = true;
    redraw_all(c);
    update_minimap(c);
}

void canvas_set_active_layer(Canvas *c, int idx) {
    if (idx >= 0 && idx < c->layer_count)
        c->active_layer = idx;
}

void canvas_toggle_layer_visible(Canvas *c, int idx) {
    if (idx < 0 || idx >= c->layer_count) return;
    c->layers[idx].visible = !c->layers[idx].visible;
    redraw_all(c);
    update_minimap(c);
}

void canvas_rename_layer(Canvas *c, int idx, const char *name) {
    if (idx < 0 || idx >= c->layer_count) return;
    snprintf(c->layers[idx].name, LAYER_NAME_LEN, "%s", name);
}

void canvas_move_layer(Canvas *c, int from, int to) {
    if (from < 0 || from >= c->layer_count) return;
    if (to < 0 || to >= c->layer_count) return;
    if (from == to) return;

    Layer tmp = c->layers[from];
    if (from < to) {
        memmove(&c->layers[from], &c->layers[from + 1],
                (to - from) * sizeof(Layer));
    } else {
        memmove(&c->layers[to + 1], &c->layers[to],
                (from - to) * sizeof(Layer));
    }
    c->layers[to] = tmp;

    // Track active layer
    if (c->active_layer == from)
        c->active_layer = to;

    c->dirty = true;
    redraw_all(c);
    update_minimap(c);
}
