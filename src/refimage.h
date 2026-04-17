#pragma once

#include <stdbool.h>
#include "raylib.h"

// A single raster reference image overlaid on the canvas.
// Coordinates are in document space (0..CANVAS_DOC_W).
typedef struct RefImage {
    int   db_id;                // -1 until persisted
    Texture2D tex;              // GPU handle
    unsigned char *png_bytes;   // malloc'd copy of PNG bytes, for DB blob
    int   png_len;
    int   src_w, src_h;         // original pixel dims
    float x, y;                 // center in document-space coords
    float scale;                // uniform (preserves aspect)
    float rotation;             // radians
    float opacity;              // 0..1
    bool  above_strokes;
} RefImage;

void refimage_init(void);
void refimage_shutdown(void);
void refimage_clear(void);                    // on painting new/load

// Adds a new image. Copies png bytes internally; uploads decoded.
// Caller retains ownership of both inputs.
void refimage_add(const unsigned char *png, int png_len, Image decoded);

// Replace the in-memory list with images loaded from DB.
// Takes ownership of `arr` and its png_bytes (each entry must use malloc'd bytes).
void refimage_load_from_db(RefImage *arr, int n);

int  refimage_count(void);
RefImage *refimage_get(int i);                // mutable; used by db save
void refimage_set_defaults(float doc_w, float doc_h);

// Draw all images with above_strokes == want_above.
// All args describe the canvas viewport so images align with doc coords.
void refimage_draw(bool want_above,
                   int canvas_x, int canvas_y,
                   float view_x, float view_y, float zoom,
                   int panel_w, int panel_h);

// Returns true if input was consumed (selection happened, handle click, etc.).
// Caller should skip normal canvas input when this returns true.
bool refimage_update(int canvas_x, int canvas_y,
                     float view_x, float view_y, float zoom,
                     int panel_w, int panel_h);

int  refimage_selected(void);         // -1 = none
void refimage_set_selected(int idx);  // clamps; -1 to deselect

// Returns 1 and clears the flag if the user just released a manipulation.
int  refimage_consume_dirty(void);

void refimage_draw_selection_overlay(int canvas_x, int canvas_y,
                                     float view_x, float view_y, float zoom);

#ifdef REFIMAGE_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

typedef struct {
    RefImage *items;
    int count;
    int capacity;
    int selected;        // -1 = none
    enum { REF_IDLE, REF_MOVING, REF_SCALING, REF_ROTATING } mode;
    float grab_dx, grab_dy;        // mouse offset from image center at grab time (doc coords)
    int   dirty_after_release;     // 1 = trigger autosave on mouse release
    int   scale_corner;          // 0..3 during REF_SCALING
    float scale_start_dist;      // doc-space distance from center to corner at grab time
    float scale_start_scale;     // r->scale at grab time
} RefImageList;

static RefImageList g_refs;

void refimage_init(void) {
    g_refs.items = NULL;
    g_refs.count = 0;
    g_refs.capacity = 0;
    g_refs.selected = -1;
    g_refs.mode = REF_IDLE;
    g_refs.grab_dx = 0;
    g_refs.grab_dy = 0;
    g_refs.dirty_after_release = 0;
    g_refs.scale_corner = 0;
    g_refs.scale_start_dist = 0;
    g_refs.scale_start_scale = 0;
}

static void free_item(RefImage *r) {
    if (r->tex.id) UnloadTexture(r->tex);
    free(r->png_bytes);
    r->tex = (Texture2D){0};
    r->png_bytes = NULL;
    r->png_len = 0;
}

void refimage_clear(void) {
    for (int i = 0; i < g_refs.count; i++) free_item(&g_refs.items[i]);
    g_refs.count = 0;
    g_refs.selected = -1;
}

void refimage_shutdown(void) {
    refimage_clear();
    free(g_refs.items);
    g_refs.items = NULL;
    g_refs.capacity = 0;
}

int refimage_count(void) { return g_refs.count; }

RefImage *refimage_get(int i) {
    if (i < 0 || i >= g_refs.count) return NULL;
    return &g_refs.items[i];
}

static void ensure_capacity(int need) {
    if (need <= g_refs.capacity) return;
    int new_cap = g_refs.capacity ? g_refs.capacity * 2 : 4;
    while (new_cap < need) new_cap *= 2;
    g_refs.items = realloc(g_refs.items, new_cap * sizeof(RefImage));
    g_refs.capacity = new_cap;
}

void refimage_add(const unsigned char *png, int png_len, Image decoded) {
    ensure_capacity(g_refs.count + 1);
    RefImage *r = &g_refs.items[g_refs.count++];
    r->db_id = -1;
    r->tex = LoadTextureFromImage(decoded);
    r->png_bytes = malloc(png_len);
    memcpy(r->png_bytes, png, png_len);
    r->png_len = png_len;
    r->src_w = decoded.width;
    r->src_h = decoded.height;
    r->x = 0; r->y = 0;              // caller should set sensible defaults
    r->scale = 1.0f;
    r->rotation = 0.0f;
    r->opacity = 1.0f;
    r->above_strokes = true;
}

void refimage_load_from_db(RefImage *arr, int n) {
    refimage_clear();
    ensure_capacity(n);
    memcpy(g_refs.items, arr, n * sizeof(RefImage));
    g_refs.count = n;
    // caller gave us ownership of png_bytes; textures were already uploaded
    free(arr);  // the array itself, not the items
}

// Set the newly-added (last) image's defaults given canvas doc dims.
void refimage_set_defaults(float doc_w, float doc_h) {
    if (g_refs.count == 0) return;
    RefImage *r = &g_refs.items[g_refs.count - 1];
    r->x = doc_w * 0.5f;
    r->y = doc_h * 0.5f;
    float fit_w = (doc_w * 0.6f) / (float)r->src_w;
    float fit_h = (doc_h * 0.6f) / (float)r->src_h;
    float fit = fit_w < fit_h ? fit_w : fit_h;
    r->scale = (fit < 1.0f) ? fit : 1.0f;
}

void refimage_draw(bool want_above,
                   int canvas_x, int canvas_y,
                   float view_x, float view_y, float zoom,
                   int panel_w, int panel_h) {
    BeginScissorMode(canvas_x, canvas_y, panel_w, panel_h);
    for (int i = 0; i < g_refs.count; i++) {
        RefImage *r = &g_refs.items[i];
        if (r->above_strokes != want_above) continue;

        float sx = canvas_x + view_x + r->x * zoom;
        float sy = canvas_y + view_y + r->y * zoom;
        float sw = (float)r->src_w * r->scale * zoom;
        float sh = (float)r->src_h * r->scale * zoom;

        Rectangle src  = {0, 0, (float)r->src_w, (float)r->src_h};
        Rectangle dest = {sx, sy, sw, sh};
        Vector2 origin = {sw * 0.5f, sh * 0.5f};
        Color tint = {255, 255, 255, (unsigned char)(r->opacity * 255.0f)};
        DrawTexturePro(r->tex, src, dest, origin,
                       r->rotation * RAD2DEG, tint);
    }
    EndScissorMode();
}

#include <math.h>

int refimage_selected(void) { return g_refs.selected; }

void refimage_set_selected(int idx) {
    if (idx < -1 || idx >= g_refs.count) g_refs.selected = -1;
    else g_refs.selected = idx;
}

// Convert screen mouse pos to document-space coord.
static void screen_to_doc(float mx, float my,
                          int canvas_x, int canvas_y,
                          float view_x, float view_y, float zoom,
                          float *out_x, float *out_y) {
    *out_x = (mx - canvas_x - view_x) / zoom;
    *out_y = (my - canvas_y - view_y) / zoom;
}

// Is point (doc-space) inside the rotated rect of image r?
static bool point_in_image(const RefImage *r, float dx, float dy) {
    float c = cosf(-r->rotation);
    float s = sinf(-r->rotation);
    float lx = (dx - r->x) * c - (dy - r->y) * s;
    float ly = (dx - r->x) * s + (dy - r->y) * c;
    float hw = r->src_w * r->scale * 0.5f;
    float hh = r->src_h * r->scale * 0.5f;
    return (lx >= -hw && lx <= hw && ly >= -hh && ly <= hh);
}

#define HANDLE_SIZE 10.0f   // screen-space px (half-side)
#define HANDLE_ROT_DIST 24.0f

// Fills 4 corner positions in screen space.
// Order: TL, TR, BR, BL.
static void corners_screen(const RefImage *r,
                           int canvas_x, int canvas_y,
                           float view_x, float view_y, float zoom,
                           Vector2 out[4]) {
    float hw = r->src_w * r->scale * 0.5f;
    float hh = r->src_h * r->scale * 0.5f;
    float c = cosf(r->rotation);
    float s = sinf(r->rotation);
    Vector2 local[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    for (int i = 0; i < 4; i++) {
        float dx = r->x + local[i].x * c - local[i].y * s;
        float dy = r->y + local[i].x * s + local[i].y * c;
        out[i].x = canvas_x + view_x + dx * zoom;
        out[i].y = canvas_y + view_y + dy * zoom;
    }
}

// Returns 0..3 if mouse m is over a corner handle, -1 otherwise.
static int hit_corner(const Vector2 corners[4], Vector2 m) {
    for (int i = 0; i < 4; i++) {
        if (fabsf(m.x - corners[i].x) <= HANDLE_SIZE &&
            fabsf(m.y - corners[i].y) <= HANDLE_SIZE)
            return i;
    }
    return -1;
}

bool refimage_update(int canvas_x, int canvas_y,
                     float view_x, float view_y, float zoom,
                     int panel_w, int panel_h) {
    (void)panel_w; (void)panel_h;
    Vector2 m = GetMousePosition();
    float dx, dy;
    screen_to_doc(m.x, m.y, canvas_x, canvas_y, view_x, view_y, zoom, &dx, &dy);

    if (g_refs.selected != -1 && IsKeyPressed(KEY_ESCAPE) && g_refs.mode == REF_IDLE) {
        g_refs.selected = -1;
        return true;
    }

    // Active drag (move)
    if (g_refs.mode == REF_MOVING && g_refs.selected >= 0) {
        RefImage *r = &g_refs.items[g_refs.selected];
        r->x = dx - g_refs.grab_dx;
        r->y = dy - g_refs.grab_dy;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            g_refs.mode = REF_IDLE;
            g_refs.dirty_after_release = 1;
        }
        return true;
    }

    // Active drag (scale)
    if (g_refs.mode == REF_SCALING && g_refs.selected >= 0) {
        RefImage *r = &g_refs.items[g_refs.selected];
        float ddx = dx - r->x;
        float ddy = dy - r->y;
        float dist = sqrtf(ddx * ddx + ddy * ddy);
        float ratio = dist / g_refs.scale_start_dist;
        r->scale = g_refs.scale_start_scale * ratio;
        if (r->scale < 0.05f) r->scale = 0.05f;
        if (r->scale > 20.0f) r->scale = 20.0f;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            g_refs.mode = REF_IDLE;
            g_refs.dirty_after_release = 1;
        }
        return true;
    }

    // Mouse press: check handles on selected, then body hit-test
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_refs.selected >= 0) {
            RefImage *r = &g_refs.items[g_refs.selected];
            Vector2 cor[4];
            corners_screen(r, canvas_x, canvas_y, view_x, view_y, zoom, cor);
            int c = hit_corner(cor, m);
            if (c >= 0) {
                g_refs.mode = REF_SCALING;
                g_refs.scale_corner = c;
                float ddx = dx - r->x;
                float ddy = dy - r->y;
                g_refs.scale_start_dist = sqrtf(ddx * ddx + ddy * ddy);
                if (g_refs.scale_start_dist < 1.0f) g_refs.scale_start_dist = 1.0f;
                g_refs.scale_start_scale = r->scale;
                return true;
            }
        }

        int hit = -1;
        for (int i = g_refs.count - 1; i >= 0; i--) {
            if (point_in_image(&g_refs.items[i], dx, dy)) { hit = i; break; }
        }
        if (hit != -1) {
            g_refs.selected = hit;
            g_refs.mode = REF_MOVING;
            RefImage *r = &g_refs.items[hit];
            g_refs.grab_dx = dx - r->x;
            g_refs.grab_dy = dy - r->y;
            return true;
        }
        if (g_refs.selected != -1) {
            g_refs.selected = -1;
            return true;
        }
    }

    return false;
}

int refimage_consume_dirty(void) {
    if (g_refs.dirty_after_release) {
        g_refs.dirty_after_release = 0;
        return 1;
    }
    return 0;
}

void refimage_draw_selection_overlay(int canvas_x, int canvas_y,
                                     float view_x, float view_y, float zoom) {
    if (g_refs.selected < 0 || g_refs.selected >= g_refs.count) return;
    RefImage *r = &g_refs.items[g_refs.selected];

    Vector2 cor[4];
    corners_screen(r, canvas_x, canvas_y, view_x, view_y, zoom, cor);

    // Rectangle outline
    for (int i = 0; i < 4; i++)
        DrawLineEx(cor[i], cor[(i + 1) % 4], 1.5f, (Color){0, 0, 0, 180});

    // Corner handles
    for (int i = 0; i < 4; i++) {
        Rectangle h = {cor[i].x - HANDLE_SIZE, cor[i].y - HANDLE_SIZE,
                       HANDLE_SIZE * 2.0f, HANDLE_SIZE * 2.0f};
        DrawRectangleRec(h, WHITE);
        DrawRectangleLinesEx(h, 1.0f, (Color){0, 0, 0, 200});
    }
}

#endif // REFIMAGE_IMPLEMENTATION
