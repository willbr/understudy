#include "canvas.h"
#include "rlgl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ── Paper texture ────────────────────────────────────────────────────────────

#define PAPER_TILE 256

static unsigned int paper_hash(int x, int y) {
    unsigned int h = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
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
    if (l->cache_w > 0) UnloadRenderTexture(l->cache_rt);
    if (l->overview_w > 0) UnloadRenderTexture(l->overview_rt);
    l->overview_w = 0;
    l->overview_valid = false;
    l->strokes         = NULL;
    l->stroke_count    = 0;
    l->stroke_capacity = 0;
    l->cache_w = l->cache_h = 0;
    l->cache_dirty = false;
}

static void layer_init(Layer *l, const char *name) {
    memset(l, 0, sizeof(*l));
    snprintf(l->name, LAYER_NAME_LEN, "%s", name);
    l->visible = true;
    l->z       = 0.0f;  // caller assigns next_z
    l->opacity = 1.0f;
}

// Layer-cache refresh policy while the view is moving (see canvas_draw_layer).
#define LAYER_CHEAP_RASTER_S       0.0015  // layers cheaper than this re-rasterize every frame
#define LAYER_SETTLE_S             0.08    // view unchanged this long = at rest
#define JOB_BUDGET_S               0.002   // background rasterization time per frame
#define JOB_SEG_CHUNK              16      // stroke segments rendered between budget checks
#define JOB_ABANDON_S              0.25    // job not advanced this long = its layer is no longer drawn
#define JOB_CACHE                  0       // job_kind: re-rasterize a layer cache at a new view
#define JOB_OVERVIEW               1       // job_kind: rebuild a layer's whole-document overview
#define OVERVIEW_SIZE              1024    // per-layer overview texture (square, covers the document)

static void invalidate_all_layer_caches(Canvas *c) {
    for (int i = 0; i < c->layer_count; i++)
        c->layers[i].cache_dirty = true;
}

// Free every layer's cache RT. Called when the panel size changes — caches
// are size-specific and would have to be reallocated anyway.
static void drop_all_layer_caches(Canvas *c) {
    for (int i = 0; i < c->layer_count; i++) {
        if (c->layers[i].cache_w > 0)
            UnloadRenderTexture(c->layers[i].cache_rt);
        c->layers[i].cache_w = c->layers[i].cache_h = 0;
        c->layers[i].cache_dirty = false;
    }
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

// Filled circle stamp — the inner loop of all stroke rendering. Same fan
// layout as raylib's DrawCircleV (36 steps) but with a precomputed table
// instead of per-vertex trig, and fewer steps for small radii where the
// polygon is within ~0.2 px of a true circle anyway.
static void stamp_circle(Vector2 center, float r, Color color) {
    static float tc[37], ts[37];
    static bool  ready = false;
    if (!ready) {
        for (int i = 0; i <= 36; i++) {
            tc[i] = cosf(DEG2RAD * 10.0f * (float)i);
            ts[i] = sinf(DEG2RAD * 10.0f * (float)i);
        }
        ready = true;
    }
    int stride = r <= 1.5f ? 6 : r <= 6.0f ? 3 : r <= 14.0f ? 2 : 1;

    rlBegin(RL_TRIANGLES);
    rlColor4ub(color.r, color.g, color.b, color.a);
    for (int i = 0; i < 36; i += stride) {
        rlVertex2f(center.x, center.y);
        rlVertex2f(center.x + tc[i + stride] * r, center.y + ts[i + stride] * r);
        rlVertex2f(center.x + tc[i] * r,          center.y + ts[i] * r);
    }
    rlEnd();
}

// Render segments [seg_from, seg_to) of a stroke (segment i joins point i to
// point i+1). Strokes too short for the spline path (1–2 points, line tool)
// ignore the range and draw whole. When clip_w/clip_h are > 0, spline segments
// that fall entirely outside that target rectangle are skipped.
static void render_stroke_range(const Stroke *s, float vx, float vy, float zoom,
                                int seg_from, int seg_to,
                                float clip_w, float clip_h) {
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
        stamp_circle((Vector2){sx0, sy0}, base_r * 0.5f, color);
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
            stamp_circle((Vector2){fsx + dx*t, fsy + dy*t}, base_r, color);
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
            stamp_circle((Vector2){fsx + dx*t, fsy + dy*t}, r, color);
        }
        goto done;
    }

    // 3+ points: Catmull-Rom spline for smooth curves
    if (seg_from < 0) seg_from = 0;
    if (seg_to > s->count - 1) seg_to = s->count - 1;
    for (int i = seg_from; i < seg_to; i++) {
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

        // Cull: the curve between p1 and p2 lies inside the hull of its
        // Bezier control points, so their bounding box (padded by r) is a
        // safe bound on everything this segment can touch.
        if (clip_w > 0.0f && clip_h > 0.0f) {
            float b1x = p1.x + (p2.x - p0.x) / 6.0f, b1y = p1.y + (p2.y - p0.y) / 6.0f;
            float b2x = p2.x - (p3.x - p1.x) / 6.0f, b2y = p2.y - (p3.y - p1.y) / 6.0f;
            float minx = fminf(fminf(p1.x, p2.x), fminf(b1x, b2x)) - r;
            float maxx = fmaxf(fmaxf(p1.x, p2.x), fmaxf(b1x, b2x)) + r;
            float miny = fminf(fminf(p1.y, p2.y), fminf(b1y, b2y)) - r;
            float maxy = fmaxf(fmaxf(p1.y, p2.y), fmaxf(b1y, b2y)) + r;
            if (maxx < 0.0f || minx > clip_w || maxy < 0.0f || miny > clip_h)
                continue;
        }

        // Interpolate along spline
        float step = fmaxf(0.5f, r * 0.25f);
        int steps = (int)(dist / step) + 1;
        if (steps < 2) steps = 2;
        for (int j = 0; j <= steps; j++) {
            float t = (float)j / (float)steps;
            Vector2 pt = catmull_rom(p0, p1, p2, p3, t);
            stamp_circle(pt, r, color);
        }
    }

done:
    if (eraser) {
        rlDrawRenderBatchActive();
        rlSetBlendMode(RL_BLEND_ALPHA);
    }
}

void render_stroke_transformed(const Stroke *s,
                               float vx, float vy, float zoom) {
    render_stroke_range(s, vx, vy, zoom, 0, s->count - 1, 0.0f, 0.0f);
}

// Exact copy of one render texture into another of the same size (no blending,
// so alpha is preserved as-is).
static void copy_rt(RenderTexture2D dst, RenderTexture2D src) {
    float w = (float)src.texture.width, h = (float)src.texture.height;
    BeginTextureMode(dst);
    ClearBackground(BLANK);
    rlDrawRenderBatchActive();
    rlSetBlendFactors(0x1, 0x0, 0x8006); // GL_ONE, GL_ZERO, GL_FUNC_ADD
    rlSetBlendMode(RL_BLEND_CUSTOM);
    DrawTexturePro(src.texture, (Rectangle){0, 0, w, -h}, (Rectangle){0, 0, w, h},
                   (Vector2){0, 0}, 0.0f, WHITE);
    rlDrawRenderBatchActive();
    rlSetBlendMode(RL_BLEND_ALPHA);
    EndTextureMode();
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

// ── Background re-rasterization job ──────────────────────────────────────────
// One layer at a time is re-rasterized into job_rt in small time slices (see
// canvas_draw_layer), so a view change never costs a whole rasterization in a
// single frame.

static void job_cancel(Canvas *c) {
    c->job_active = false;
}

// Scale from layer-local document coordinates to overview texels.
static float overview_scale(const Canvas *c) {
    int max_dim = c->width > c->height ? c->width : c->height;
    return (float)OVERVIEW_SIZE / (float)max_dim;
}

// Mipmaps let the overview be minified cleanly when zoomed far out.
static void overview_finish(Layer *l) {
    GenTextureMipmaps(&l->overview_rt.texture);
    SetTextureFilter(l->overview_rt.texture, TEXTURE_FILTER_TRILINEAR);
    l->overview_valid = true;
}

static void overview_alloc(Layer *l) {
    if (l->overview_w > 0) return;
    l->overview_rt = LoadRenderTexture(OVERVIEW_SIZE, OVERVIEW_SIZE);
    l->overview_w  = OVERVIEW_SIZE;
    l->overview_valid = false;
}

// Which layer should get the job next, and for what. Sharp caches come first:
// the visible, expensive layer whose cache is out of date for the current
// view and has been so the longest. Failing that, an expensive layer that has
// no up-to-date overview.
static int job_candidate(const Canvas *c, int *kind) {
    int pw = c->strokes_rt.texture.width, ph = c->strokes_rt.texture.height;
    int best = -1, best_ov = -1;
    for (int i = 0; i < c->layer_count; i++) {
        const Layer *l = &c->layers[i];
        if (!l->visible || l->stroke_count == 0 || l->cache_dirty) continue;
        if (l->cache_w != pw || l->cache_h != ph) continue;
        if (l->cache_cost < LAYER_CHEAP_RASTER_S) continue;
        if (!l->overview_valid && best_ov < 0) best_ov = i;
        float pvx = c->view_x + l->pan_x * c->zoom;
        float pvy = c->view_y + l->pan_y * c->zoom;
        if (l->cache_vx == pvx && l->cache_vy == pvy && l->cache_zoom == c->zoom) continue;
        if (best < 0 || l->cache_time < c->layers[best].cache_time) best = i;
    }
    *kind = best >= 0 ? JOB_CACHE : JOB_OVERVIEW;
    return best >= 0 ? best : best_ov;
}

// Spend up to JOB_BUDGET_S rendering the next strokes of the job's layer into
// the job's target. Returns true when the layer is fully rendered.
static bool job_advance(Canvas *c, Layer *l, float clip_w, float clip_h) {
    double t0 = GetTime();
    BeginTextureMode(c->job_kind == JOB_OVERVIEW ? c->job_ov_rt : c->job_rt);
    if (!c->job_started) {
        ClearBackground(BLANK);
        c->job_started = true;
    }
    while (c->job_stroke < l->stroke_count) {
        const Stroke *s = &l->strokes[c->job_stroke];
        int nseg = s->count - 1;
        if (s->count < 3) {
            // Short strokes ignore the segment range and draw whole.
            render_stroke_range(s, c->job_vx, c->job_vy, c->job_zoom, 0, nseg, clip_w, clip_h);
            c->job_seg = nseg;
        } else {
            int to = c->job_seg + JOB_SEG_CHUNK;
            if (to > nseg) to = nseg;
            render_stroke_range(s, c->job_vx, c->job_vy, c->job_zoom, c->job_seg, to, clip_w, clip_h);
            c->job_seg = to;
        }
        if (c->job_seg >= nseg) {
            c->job_stroke++;
            c->job_seg = 0;
        }
        if (GetTime() - t0 > JOB_BUDGET_S) break;
    }
    EndTextureMode();
    c->job_touch  = GetTime();
    c->job_spent += c->job_touch - t0;
    return c->job_stroke >= l->stroke_count;
}

// Drop the in-progress stroke's incremental render (see canvas_draw_layer).
// Called whenever layer contents or the layer list change, so it also stops
// any background job, which would otherwise be rendering outdated content.
static void live_invalidate(Canvas *c) {
    c->live_valid = false;
    c->live_segs  = 0;
    job_cancel(c);
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
        float mpx = l->pan_x * ms;
        float mpy = l->pan_y * ms;
        for (int si = 0; si < l->stroke_count; si++)
            render_stroke_transformed(&l->strokes[si], mpx, mpy, ms);
    }
    EndTextureMode();
}

// (Re)allocate live_rt and job_rt at the panel size up front, so the first
// stroke or view change doesn't pay for the allocation mid-interaction.
static void alloc_work_rts(Canvas *c, int w, int h) {
    if (c->live_w > 0) UnloadRenderTexture(c->live_rt);
    if (c->job_w > 0)  UnloadRenderTexture(c->job_rt);
    c->live_rt = LoadRenderTexture(w, h);
    c->job_rt  = LoadRenderTexture(w, h);
    // Either may be swapped in as a layer cache, which is bilinear.
    SetTextureFilter(c->live_rt.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(c->job_rt.texture, TEXTURE_FILTER_BILINEAR);
    c->live_w = c->job_w = w;
    c->live_h = c->job_h = h;
    live_invalidate(c);
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
    c->paper_rt     = LoadRenderTexture(CANVAS_WIDTH, CANVAS_HEIGHT);
    c->minimap_rt   = LoadRenderTexture(MINIMAP_SIZE, MINIMAP_SIZE);
    c->live_w = c->live_h = 0;
    c->live_layer = -1;
    c->job_w = c->job_h = 0;
    c->job_layer = -1;
    alloc_work_rts(c, CANVAS_WIDTH, CANVAS_HEIGHT);
    c->job_ov_rt = LoadRenderTexture(OVERVIEW_SIZE, OVERVIEW_SIZE);
    c->job_kind  = JOB_CACHE;

    memset(c->layers, 0, sizeof(c->layers));
    c->layer_count  = 1;
    c->active_layer = 0;
    c->next_z       = 0.0f;
    layer_init(&c->layers[0], "Background");
    c->layers[0].z  = c->next_z++;

    memset(&c->current, 0, sizeof(c->current));
    c->is_drawing = false;
    c->dirty      = false;

    reset_view(c);
    render_paper(c);
    update_minimap(c);
}

void canvas_free(Canvas *c) {
    UnloadShader(c->ink_shader);
    UnloadShader(c->paper_shader);
    UnloadTexture(c->paper_tex);
    UnloadRenderTexture(c->minimap_rt);
    UnloadRenderTexture(c->paper_rt);
    UnloadRenderTexture(c->strokes_rt);
    UnloadRenderTexture(c->rt);
    if (c->live_w > 0) UnloadRenderTexture(c->live_rt);
    if (c->job_w > 0)  UnloadRenderTexture(c->job_rt);
    UnloadRenderTexture(c->job_ov_rt);
    for (int i = 0; i < c->layer_count; i++)
        layer_free(&c->layers[i]);
    stroke_free_data(&c->current);
}

void canvas_begin_stroke(Canvas *c, Color color, int radius, ToolType tool) {
    stroke_free_data(&c->current);
    c->live_valid = false;
    c->live_segs  = 0;
    c->current.color  = color;
    c->current.radius = radius;
    c->current.tool   = tool;
    c->is_drawing     = true;
}

void canvas_add_point(Canvas *c, Vector2 p) {
    if (!c->is_drawing) return;

    // Convert from doc coords to layer-local coords (subtract active layer pan)
    Layer *al = &c->layers[c->active_layer];
    p.x -= al->pan_x;
    p.y -= al->pan_y;

    Stroke *s = &c->current;
    if (s->count >= s->capacity) {
        int newcap = s->capacity == 0 ? 64 : s->capacity * 2;
        Vector2 *tmp = realloc(s->points, newcap * sizeof(Vector2));
        if (!tmp) return;
        s->points   = tmp;
        s->capacity = newcap;
    }
    s->points[s->count++] = p;
    c->dirty = true;
    // No rendering here — canvas_draw_layer handles the live preview each
    // frame, rendering only the segments added since the previous frame.
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
    // Minimap fast path: an additive (non-eraser) stroke on the topmost
    // visible layer can simply be stamped on top of the existing thumbnail.
    bool brush_fast_path = c->current.tool != TOOL_ERASER;
    if (brush_fast_path) {
        for (int li = c->active_layer + 1; li < c->layer_count; li++) {
            if (c->layers[li].visible) { brush_fast_path = false; break; }
        }
    }

    l->strokes[l->stroke_count++] = c->current;
    memset(&c->current, 0, sizeof(c->current));
    c->is_drawing = false;
    Stroke *s = &l->strokes[l->stroke_count - 1];

    // Fold the new stroke into the layer's raster cache instead of marking the
    // cache dirty (which would re-rasterize every stroke in the layer). Strokes
    // render in order, so "cache + new stroke on top" is exactly what a full
    // re-rasterization would produce. Only valid if the cache is current.
    int   pw  = c->strokes_rt.texture.width;
    int   ph  = c->strokes_rt.texture.height;
    float pvx = c->view_x + l->pan_x * c->zoom;
    float pvy = c->view_y + l->pan_y * c->zoom;
    bool cache_fresh = l->cache_w == pw && l->cache_h == ph && !l->cache_dirty &&
                       l->cache_vx == pvx && l->cache_vy == pvy &&
                       l->cache_zoom == c->zoom;
    // Folding also has to keep cache_cost (what a full re-rasterization of
    // this layer would take) up to date, or a layer built up stroke by stroke
    // would still look "cheap" and get rasterized in one go — a stall — on
    // the first view change.
    double t0 = GetTime();
    if (!cache_fresh) {
        l->cache_dirty = true;
    } else if (c->live_valid && c->live_layer == c->active_layer &&
               c->live_w == pw && c->live_h == ph) {
        // live_rt already holds cache + the settled part of this stroke:
        // finish the tail there and swap it in as the new cache.
        BeginTextureMode(c->live_rt);
        render_stroke_range(s, pvx, pvy, c->zoom, c->live_segs, s->count - 1,
                            (float)pw, (float)ph);
        EndTextureMode();
        RenderTexture2D tmp = l->cache_rt;
        l->cache_rt = c->live_rt;
        c->live_rt  = tmp;
        l->cache_cost += c->live_spent + (GetTime() - t0);
    } else {
        BeginTextureMode(l->cache_rt);
        render_stroke_range(s, pvx, pvy, c->zoom, 0, s->count - 1,
                            (float)pw, (float)ph);
        EndTextureMode();
        l->cache_cost += GetTime() - t0;
    }
    c->live_valid = false;
    c->live_segs  = 0;

    // Same idea for the overview: stamp the new stroke onto it. A layer's
    // first stroke creates it; an overview that is out of date is left for
    // the background job, which picks this stroke up as it rebuilds.
    if (cache_fresh) {
        if (l->overview_w == 0 && l->stroke_count == 1) {
            overview_alloc(l);
            BeginTextureMode(l->overview_rt);
            ClearBackground(BLANK);
            EndTextureMode();
            l->overview_valid = true;
        }
        if (l->overview_valid) {
            BeginTextureMode(l->overview_rt);
            render_stroke_range(s, 0.0f, 0.0f, overview_scale(c), 0, s->count - 1,
                                (float)OVERVIEW_SIZE, (float)OVERVIEW_SIZE);
            EndTextureMode();
            overview_finish(l);
        }
    }

    if (brush_fast_path) {
        // Stamp only the new stroke onto the minimap.
        int max_dim = c->width > c->height ? c->width : c->height;
        float ms = (float)MINIMAP_SIZE / (float)max_dim;
        BeginTextureMode(c->minimap_rt);
        render_stroke_transformed(s, l->pan_x * ms, l->pan_y * ms, ms);
        EndTextureMode();
    } else {
        update_minimap(c);
    }
}

void canvas_undo(Canvas *c) {
    if (c->is_drawing) {
        stroke_free_data(&c->current);
        c->is_drawing = false;
        update_minimap(c);
        return;
    }
    Layer *l = active_layer(c);
    if (l->stroke_count == 0) return;
    stroke_free_data(&l->strokes[--l->stroke_count]);
    l->cache_dirty = true;
    update_minimap(c);
    c->dirty = (total_strokes(c) > 0);
}

void canvas_clear(Canvas *c) {
    for (int i = 0; i < c->layer_count; i++)
        layer_free(&c->layers[i]);
    c->layer_count  = 1;
    live_invalidate(c);
    c->active_layer = 0;
    c->next_z       = 0.0f;
    layer_init(&c->layers[0], "Background");
    c->layers[0].z  = c->next_z++;

    stroke_free_data(&c->current);
    c->is_drawing = false;
    c->dirty      = false;

    reset_view(c);
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
    live_invalidate(c);
    c->active_layer = 0;
    c->dirty        = false;

    free(layers);  // free the container (layer contents now owned by c->layers[])

    // Set next_z to max loaded z + 1 so new layers/refs don't collide
    float max_z = 0.0f;
    for (int i = 0; i < c->layer_count; i++) {
        if (c->layers[i].z > max_z) max_z = c->layers[i].z;
    }
    c->next_z = max_z + 1.0f;

    reset_view(c);
    update_minimap(c);
}

bool canvas_export_png(Canvas *c, const char *path, int scale) {
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    int ew = c->width * scale, eh = c->height * scale;

    // Render at 2x target resolution for anti-aliasing (supersampling)
    int ssw = ew * 2, ssh = eh * 2;
    float ss_zoom = (float)(scale * 2);

    // Per-layer compositing so opacity is respected in exports
    RenderTexture2D layer_rt  = LoadRenderTexture(ssw, ssh);  // scratch per layer
    RenderTexture2D layer_ink = LoadRenderTexture(ssw, ssh);  // ink-composited layer
    RenderTexture2D ss_rt     = LoadRenderTexture(ssw, ssh);  // accumulated result

    float docSize[2] = {(float)c->width, (float)c->height};
    float viewOff[2] = {0.0f, 0.0f};
    float res[2]     = {(float)ssw, (float)ssh};
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "docSize"),    docSize,   SHADER_UNIFORM_VEC2);
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "viewOffset"), viewOff,   SHADER_UNIFORM_VEC2);
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "zoom"),       &ss_zoom,  SHADER_UNIFORM_FLOAT);
    SetShaderValue(c->ink_shader, GetShaderLocation(c->ink_shader, "resolution"), res,       SHADER_UNIFORM_VEC2);

    // Start with a white paper background
    BeginTextureMode(ss_rt);
    ClearBackground(WHITE);
    EndTextureMode();

    Rectangle ss_src  = {0, 0, (float)ssw, -(float)ssh};
    Rectangle ss_dest = {0, 0, (float)ssw,  (float)ssh};

    for (int li = 0; li < c->layer_count; li++) {
        if (!c->layers[li].visible) continue;
        Layer *l = &c->layers[li];
        if (l->stroke_count == 0) continue;

        float epx = l->pan_x * ss_zoom;
        float epy = l->pan_y * ss_zoom;

        // Render this layer's strokes to scratch RT
        BeginTextureMode(layer_rt);
        ClearBackground(BLANK);
        for (int si = 0; si < l->stroke_count; si++)
            render_stroke_transformed(&l->strokes[si], epx, epy, ss_zoom);
        EndTextureMode();

        // Composite strokes through ink shader into layer_ink
        BeginTextureMode(layer_ink);
        ClearBackground(BLANK);
        BeginShaderMode(c->ink_shader);
        DrawTexturePro(layer_rt.texture, ss_src, ss_dest, (Vector2){0, 0}, 0.0f, WHITE);
        EndShaderMode();
        EndTextureMode();

        // Blend layer_ink onto ss_rt with per-layer opacity
        Color tint = {255, 255, 255, (unsigned char)(l->opacity * 255.0f)};
        BeginTextureMode(ss_rt);
        DrawTexturePro(layer_ink.texture, ss_src, ss_dest, (Vector2){0, 0}, 0.0f, tint);
        EndTextureMode();
    }

    UnloadRenderTexture(layer_rt);
    UnloadRenderTexture(layer_ink);

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
    invalidate_all_layer_caches(c);
    reset_view(c);
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
    invalidate_all_layer_caches(c);
    reset_view(c);
    update_minimap(c);
}

void canvas_redraw_for_view(Canvas *c) {
    // Layer caches are keyed on the view they were rasterized at, so
    // canvas_draw_layer notices the change itself and decides whether to
    // re-rasterize now or reuse the stale cache while the view is moving.
    render_paper(c);
}

void canvas_update_minimap(Canvas *c) {
    update_minimap(c);
}

void canvas_resize(Canvas *c, int panel_w, int panel_h) {
    UnloadRenderTexture(c->rt);
    UnloadRenderTexture(c->strokes_rt);
    UnloadRenderTexture(c->paper_rt);
    drop_all_layer_caches(c);
    c->rt           = LoadRenderTexture(panel_w, panel_h);
    c->strokes_rt   = LoadRenderTexture(panel_w, panel_h);
    c->paper_rt     = LoadRenderTexture(panel_w, panel_h);
    alloc_work_rts(c, panel_w, panel_h);
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
    live_invalidate(c);

    char name[LAYER_NAME_LEN];
    snprintf(name, sizeof(name), "Layer %d", c->layer_count);
    layer_init(&c->layers[idx], name);
    c->layers[idx].z = c->next_z++;

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
    live_invalidate(c);

    if (c->active_layer >= c->layer_count)
        c->active_layer = c->layer_count - 1;
    if (c->active_layer > idx && c->active_layer > 0)
        c->active_layer--;

    c->dirty = true;
    update_minimap(c);
}

void canvas_set_active_layer(Canvas *c, int idx) {
    if (idx >= 0 && idx < c->layer_count)
        c->active_layer = idx;
}

void canvas_toggle_layer_visible(Canvas *c, int idx) {
    if (idx < 0 || idx >= c->layer_count) return;
    c->layers[idx].visible = !c->layers[idx].visible;
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
    live_invalidate(c);

    // Track active layer
    if (c->active_layer == from)
        c->active_layer = to;

    c->dirty = true;
    update_minimap(c);
}

void canvas_bump_next_z_to(Canvas *c, float z) {
    if (z + 1.0f > c->next_z) c->next_z = z + 1.0f;
}

void canvas_draw_layer(Canvas *c, int li, int x_offset) {
    if (li < 0 || li >= c->layer_count) return;
    Layer *l = &c->layers[li];
    bool active_with_inprogress =
        (li == c->active_layer) && c->is_drawing && c->current.count > 0;
    if (!l->visible) return;
    if (l->stroke_count == 0 && !active_with_inprogress) return;

    int pw = c->strokes_rt.texture.width;
    int ph = c->strokes_rt.texture.height;

    // Apply per-layer pan offset
    float pvx = c->view_x + l->pan_x * c->zoom;
    float pvy = c->view_y + l->pan_y * c->zoom;

    // Dirty because the strokes changed (as opposed to the cache merely being
    // reallocated just below).
    bool content_dirty = l->cache_dirty;

    // Ensure the per-layer cache RT matches the current panel size.
    if (l->cache_w != pw || l->cache_h != ph) {
        if (l->cache_w > 0) UnloadRenderTexture(l->cache_rt);
        l->cache_rt = LoadRenderTexture(pw, ph);
        // Bilinear so the cache scales smoothly when reused mid-zoom; a 1:1
        // blit samples texel centres and is unaffected.
        SetTextureFilter(l->cache_rt.texture, TEXTURE_FILTER_BILINEAR);
        l->cache_w = pw;
        l->cache_h = ph;
        l->cache_dirty = true;
    }

    // The cache is a raster of the layer at one particular view. `stale` = it
    // was rasterized at a different view than the current one; `moving` = the
    // view changed since this layer was last drawn (pan/zoom in progress).
    bool stale  = l->cache_vx != pvx || l->cache_vy != pvy || l->cache_zoom != c->zoom;
    // "At rest" needs a short quiet period, not just one unchanged frame: mouse
    // reports arrive slower than frames, so mid-drag frames often see no delta.
    double now = GetTime();
    if (l->seen_vx != pvx || l->seen_vy != pvy || l->seen_zoom != c->zoom)
        l->move_time = now;
    l->seen_vx = pvx; l->seen_vy = pvy; l->seen_zoom = c->zoom;
    bool moving = (now - l->move_time) < LAYER_SETTLE_S;

    // Re-rasterizing costs time proportional to the layer's stroke count, so
    // doing it in one go on a pan/zoom frame stalls that frame. Content
    // changes (cache_dirty) and a stroke in progress need an exact cache now
    // and rasterize synchronously, as do cheap layers. An expensive layer
    // whose view changed instead shows its stale cache (translated/scaled
    // below) while a background job re-rasterizes it a few ms per frame into
    // job_rt, which is swapped in when complete.
    //
    // Each expensive layer also keeps a low-resolution overview of the whole
    // document (overview_rt). The stale cache only covers the old viewport, so
    // after zooming out or panning the overview fills in everything around it
    // until the sharp raster arrives. Overviews are kept current by stamping
    // each new stroke onto them (canvas_end_stroke) and are rebuilt by the
    // same background job after any other content change.
    if (content_dirty) {
        l->overview_valid = false;               // content changed some other way
        if (c->job_layer == li) job_cancel(c);   // job was rendering old content
    }
    bool expensive = l->cache_cost >= LAYER_CHEAP_RASTER_S;
    bool rasterize = l->cache_dirty ||
                     (stale && (active_with_inprogress || !expensive));
    bool want_cache_job    = !rasterize && stale;
    bool want_overview_job = !rasterize && expensive && !l->overview_valid;
    if (want_cache_job || want_overview_job) {
        if (c->job_w != pw || c->job_h != ph) {
            if (c->job_w > 0) UnloadRenderTexture(c->job_rt);
            c->job_rt = LoadRenderTexture(pw, ph);
            SetTextureFilter(c->job_rt.texture, TEXTURE_FILTER_BILINEAR);
            c->job_w = pw;
            c->job_h = ph;
            job_cancel(c);
        }
        // A job whose layer stopped being drawn (hidden) is abandoned.
        bool busy = c->job_active && (now - c->job_touch) < JOB_ABANDON_S;
        // Sharp caches matter more than overviews: a waiting cache job
        // preempts an overview rebuild (which simply restarts later).
        if (busy && c->job_kind == JOB_OVERVIEW && want_cache_job) busy = false;
        bool mine = busy && c->job_layer == li;
        // A cache job aimed at a view we have since left, now that the view
        // has come to rest: retarget it at the final view.
        if (mine && c->job_kind == JOB_CACHE && !moving &&
            (c->job_vx != pvx || c->job_vy != pvy || c->job_zoom != c->zoom)) {
            mine = false;
            busy = false;
        }
        int kind = JOB_CACHE;
        if (!busy && job_candidate(c, &kind) == li) {
            c->job_active  = true;
            c->job_started = false;
            c->job_kind    = kind;
            c->job_layer   = li;
            c->job_stroke  = 0;
            c->job_seg     = 0;
            c->job_spent   = 0.0;
            if (kind == JOB_CACHE) {
                c->job_vx = pvx; c->job_vy = pvy; c->job_zoom = c->zoom;
            } else {
                c->job_vx = 0.0f; c->job_vy = 0.0f; c->job_zoom = overview_scale(c);
            }
            mine = true;
        }
        if (mine && c->job_kind == JOB_CACHE) {
            if (job_advance(c, l, (float)pw, (float)ph)) {
                RenderTexture2D tmp = l->cache_rt;
                l->cache_rt = c->job_rt;
                c->job_rt   = tmp;
                l->cache_vx = c->job_vx; l->cache_vy = c->job_vy; l->cache_zoom = c->job_zoom;
                l->cache_time = GetTime();
                l->cache_cost = c->job_spent;
                job_cancel(c);
                stale = l->cache_vx != pvx || l->cache_vy != pvy || l->cache_zoom != c->zoom;
            }
        } else if (mine) {
            if (job_advance(c, l, (float)OVERVIEW_SIZE, (float)OVERVIEW_SIZE)) {
                overview_alloc(l);
                RenderTexture2D tmp = l->overview_rt;
                l->overview_rt = c->job_ov_rt;
                c->job_ov_rt   = tmp;
                overview_finish(l);
                job_cancel(c);
            }
        }
    }
    if (rasterize) {
        // A cache job for this layer is now redundant.
        if (c->job_active && c->job_layer == li && c->job_kind == JOB_CACHE)
            job_cancel(c);
        double t0 = GetTime();
        BeginTextureMode(l->cache_rt);
        ClearBackground(BLANK);
        for (int si = 0; si < l->stroke_count; si++)
            render_stroke_range(&l->strokes[si], pvx, pvy, c->zoom,
                                0, l->strokes[si].count - 1, (float)pw, (float)ph);
        EndTextureMode();
        l->cache_dirty = false;
        l->cache_vx = pvx; l->cache_vy = pvy; l->cache_zoom = c->zoom;
        l->cache_time = GetTime();
        l->cache_cost = l->cache_time - t0;
        stale = false;
        if (c->live_layer == li) live_invalidate(c);  // was built on the old cache
    }

    // In-progress freehand stroke: instead of re-rendering the whole stroke
    // every frame (cost grows with stroke length), keep live_rt = cache + the
    // "settled" part of the stroke, and add only newly settled segments each
    // frame. A segment is settled once 3 more points follow it: from then on
    // neither its spline control points nor its end taper can change. Only the
    // short unsettled tail is redrawn per frame.
    bool use_live = active_with_inprogress && c->current.tool != TOOL_LINE &&
                    c->current.count >= 3;
    int settled = 0;
    if (use_live) {
        if (c->live_w != pw || c->live_h != ph) {
            if (c->live_w > 0) UnloadRenderTexture(c->live_rt);
            c->live_rt = LoadRenderTexture(pw, ph);
            SetTextureFilter(c->live_rt.texture, TEXTURE_FILTER_BILINEAR);  // may be swapped in as a cache
            c->live_w = pw;
            c->live_h = ph;
            live_invalidate(c);
        }
        if (!c->live_valid || c->live_layer != li) {
            copy_rt(c->live_rt, l->cache_rt);
            c->live_valid = true;
            c->live_layer = li;
            c->live_segs  = 0;
            c->live_spent = 0.0;
        }
        settled = c->current.count - 3;
        if (settled > c->live_segs) {
            double t0 = GetTime();
            BeginTextureMode(c->live_rt);
            render_stroke_range(&c->current, pvx, pvy, c->zoom,
                                c->live_segs, settled, (float)pw, (float)ph);
            EndTextureMode();
            c->live_segs = settled;
            c->live_spent += GetTime() - t0;
        }
    }

    // Build this layer's strokes_rt: cache (or live) + whatever is still in flux.
    BeginTextureMode(c->strokes_rt);
    ClearBackground(BLANK);
    Rectangle cache_src = {0, 0, (float)pw, -(float)ph};
    if (use_live) {
        DrawTexturePro(c->live_rt.texture, cache_src,
                       (Rectangle){0, 0, (float)pw, (float)ph},
                       (Vector2){0, 0}, 0.0f, WHITE);
        render_stroke_range(&c->current, pvx, pvy, c->zoom,
                            settled, c->current.count - 1, (float)pw, (float)ph);
    } else {
        // Map the cache from the view it was rasterized at to the current one
        // (identity when the cache is fresh).
        Rectangle cache_dest = {0, 0, (float)pw, (float)ph};
        if (stale) {
            float s = c->zoom / l->cache_zoom;
            cache_dest = (Rectangle){pvx - l->cache_vx * s, pvy - l->cache_vy * s,
                                     (float)pw * s, (float)ph * s};
        }
        if (stale && l->overview_valid) {
            // Whole-document overview first, so nothing is blank...
            float ms   = overview_scale(c);
            float span = (float)OVERVIEW_SIZE / ms * c->zoom;
            DrawTexturePro(l->overview_rt.texture,
                           (Rectangle){0, 0, OVERVIEW_SIZE, -(float)OVERVIEW_SIZE},
                           (Rectangle){pvx, pvy, span, span},
                           (Vector2){0, 0}, 0.0f, WHITE);
            // ...then the stale cache replaces it (no blending) over the area
            // it covers — unless it was rasterized more zoomed-out than the
            // overview, in which case the overview is the sharper of the two.
            if (l->cache_zoom >= ms) {
                rlDrawRenderBatchActive();
                rlSetBlendFactors(0x1, 0x0, 0x8006); // GL_ONE, GL_ZERO, GL_FUNC_ADD
                rlSetBlendMode(RL_BLEND_CUSTOM);
                DrawTexturePro(l->cache_rt.texture, cache_src, cache_dest,
                               (Vector2){0, 0}, 0.0f, WHITE);
                rlDrawRenderBatchActive();
                rlSetBlendMode(RL_BLEND_ALPHA);
            }
        } else {
            DrawTexturePro(l->cache_rt.texture, cache_src, cache_dest,
                           (Vector2){0, 0}, 0.0f, WHITE);
        }
        if (active_with_inprogress)
            render_stroke_transformed(&c->current, pvx, pvy, c->zoom);
    }
    EndTextureMode();

    // Apply ink shader → rt
    composite_ink(c);

    // Blit rt to screen, applying per-layer opacity
    Color tint = {255, 255, 255, (unsigned char)(l->opacity * 255.0f)};
    Rectangle src  = {0, 0, (float)pw, -(float)ph};
    Rectangle dest = {(float)x_offset, CANVAS_Y, (float)pw, (float)ph};
    DrawTexturePro(c->rt.texture, src, dest, (Vector2){0, 0}, 0.0f, tint);
}
