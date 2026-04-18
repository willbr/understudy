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
    float z;                    // unified z-order; higher = rendered on top
    char  name[64];    // user-editable label
    bool  locked;      // true = skip hit-test
    bool  visible;     // true = render
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

// Set the newly-added (last) image's name. Truncated to 63 chars.
void refimage_set_last_name(const char *name);

// Set the z of the newly-added (last) image.
void refimage_set_last_z(float z);

// Get/set z of a specific ref by index.
float refimage_get_z(int idx);
void  refimage_set_z(int idx, float z);

// Draw a single reference image by index.
void refimage_draw_one(int ri, int canvas_x, int canvas_y,
                       float view_x, float view_y, float zoom);

// Returns true if input was consumed (selection happened, handle click, etc.).
// Caller should skip normal canvas input when this returns true.
bool refimage_update(int canvas_x, int canvas_y,
                     float view_x, float view_y, float zoom,
                     int panel_w, int panel_h);

int  refimage_selected(void);         // -1 = none
void refimage_set_selected(int idx);  // clamps; -1 to deselect

// Returns 1 and clears the flag if the user just released a manipulation.
int  refimage_consume_dirty(void);

// Toolbar-facing ops.
void refimage_rename(int idx, const char *name);         // clamps to 63 chars
void refimage_toggle_locked(int idx);
void refimage_toggle_visible(int idx);
void refimage_select(int idx);                            // no-op if out of range; -1 to deselect

// (Move up/down logic is now handled in toolbar.c using z-swap)

// Inline rename state (managed by whoever draws the refs panel).
// Returns true iff an image is currently being renamed via inline UI.
bool refimage_rename_active(void);
int  refimage_rename_index(void);
void refimage_rename_begin(int idx);
void refimage_rename_commit(void);
void refimage_rename_cancel(void);
char *refimage_rename_buffer(void);  // mutable pointer into internal 64-char buffer
int   refimage_rename_buffer_len(void);
void  refimage_rename_buffer_set_len(int len);

void refimage_draw_selection_overlay(int canvas_x, int canvas_y,
                                     float view_x, float view_y, float zoom);

void refimage_draw_panel(int canvas_x, int canvas_y,
                        float view_x, float view_y, float zoom,
                        int panel_w, int panel_h);

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
    float rot_start_angle;       // mouse angle at grab time
    float rot_start_rot;         // r->rotation at grab time
    bool slider_dragging;        // mouse held on opacity slider
    int   renaming_idx;           // -1 = no rename active
    char  rename_buf[64];
    int   rename_buf_len;
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
    g_refs.rot_start_angle = 0;
    g_refs.rot_start_rot = 0;
    g_refs.slider_dragging = false;
    g_refs.renaming_idx = -1;
    g_refs.rename_buf[0] = '\0';
    g_refs.rename_buf_len = 0;
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

float refimage_get_z(int idx) {
    if (idx < 0 || idx >= g_refs.count) return 0.0f;
    return g_refs.items[idx].z;
}

void refimage_set_z(int idx, float z) {
    if (idx < 0 || idx >= g_refs.count) return;
    g_refs.items[idx].z = z;
    g_refs.dirty_after_release = 1;
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
    r->z = 0.0f;          // caller sets via refimage_set_last_z
    r->name[0] = '\0';   // caller should set via refimage_set_last_name
    r->locked  = false;
    r->visible = true;
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

void refimage_set_last_name(const char *name) {
    if (g_refs.count == 0 || !name) return;
    RefImage *r = &g_refs.items[g_refs.count - 1];
    size_t n = strlen(name);
    if (n >= sizeof(r->name)) n = sizeof(r->name) - 1;
    memcpy(r->name, name, n);
    r->name[n] = '\0';
}

void refimage_set_last_z(float z) {
    if (g_refs.count == 0) return;
    g_refs.items[g_refs.count - 1].z = z;
}

void refimage_draw_one(int ri, int canvas_x, int canvas_y,
                       float view_x, float view_y, float zoom) {
    if (ri < 0 || ri >= g_refs.count) return;
    RefImage *r = &g_refs.items[ri];
    if (!r->visible) return;

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

// Rotation handle position in screen space: centered above the top edge midpoint,
// offset along the image's up-axis (rotated).
static Vector2 rotation_handle_screen(const RefImage *r,
                                      int canvas_x, int canvas_y,
                                      float view_x, float view_y, float zoom) {
    float hh = r->src_h * r->scale * 0.5f;
    // Image-local offset: straight up from top edge mid (in doc units);
    // add HANDLE_ROT_DIST as screen-pixel gap (/ zoom to convert)
    float local_y = -(hh + HANDLE_ROT_DIST / zoom);
    float c = cosf(r->rotation);
    float s = sinf(r->rotation);
    float dx = r->x + 0 * c - local_y * s;
    float dy = r->y + 0 * s + local_y * c;
    return (Vector2){canvas_x + view_x + dx * zoom,
                     canvas_y + view_y + dy * zoom};
}

static bool hit_rotation_handle(const RefImage *r,
                                int canvas_x, int canvas_y,
                                float view_x, float view_y, float zoom,
                                Vector2 m) {
    Vector2 h = rotation_handle_screen(r, canvas_x, canvas_y,
                                       view_x, view_y, zoom);
    float dx = m.x - h.x, dy = m.y - h.y;
    return dx * dx + dy * dy <= 12.0f * 12.0f;
}

#define PANEL_W 220.0f
#define PANEL_H 80.0f

// AABB of the selected image in screen space; fills min/max corners.
static void selection_aabb(const RefImage *r,
                           int canvas_x, int canvas_y,
                           float view_x, float view_y, float zoom,
                           float *min_x, float *min_y,
                           float *max_x, float *max_y) {
    Vector2 cor[4];
    corners_screen(r, canvas_x, canvas_y, view_x, view_y, zoom, cor);
    *min_x = cor[0].x; *min_y = cor[0].y;
    *max_x = cor[0].x; *max_y = cor[0].y;
    for (int i = 1; i < 4; i++) {
        if (cor[i].x < *min_x) *min_x = cor[i].x;
        if (cor[i].y < *min_y) *min_y = cor[i].y;
        if (cor[i].x > *max_x) *max_x = cor[i].x;
        if (cor[i].y > *max_y) *max_y = cor[i].y;
    }
}

static Rectangle panel_rect(const RefImage *r,
                            int canvas_x, int canvas_y,
                            float view_x, float view_y, float zoom,
                            int panel_w, int panel_h) {
    float mnx, mny, mxx, mxy;
    selection_aabb(r, canvas_x, canvas_y, view_x, view_y, zoom,
                   &mnx, &mny, &mxx, &mxy);
    float px = (mnx + mxx) * 0.5f - PANEL_W * 0.5f;
    float py = mxy + 12.0f;
    if (py + PANEL_H > canvas_y + panel_h) {
        py = mny - 12.0f - PANEL_H;
    }
    if (px < canvas_x + 4) px = canvas_x + 4;
    if (px + PANEL_W > canvas_x + panel_w - 4) px = canvas_x + panel_w - PANEL_W - 4;
    if (py < canvas_y + 4) py = canvas_y + 4;
    return (Rectangle){px, py, PANEL_W, PANEL_H};
}

bool refimage_update(int canvas_x, int canvas_y,
                     float view_x, float view_y, float zoom,
                     int panel_w, int panel_h) {
    Vector2 m = GetMousePosition();
    float dx, dy;
    screen_to_doc(m.x, m.y, canvas_x, canvas_y, view_x, view_y, zoom, &dx, &dy);

    bool mouse_in_panel =
        m.x >= canvas_x && m.x < canvas_x + panel_w &&
        m.y >= canvas_y && m.y < canvas_y + panel_h;

    // Cursor hint when hovering handles / body of selection (idle only)
    if (g_refs.selected >= 0 && g_refs.mode == REF_IDLE) {
        RefImage *r = &g_refs.items[g_refs.selected];
        Vector2 cor[4];
        corners_screen(r, canvas_x, canvas_y, view_x, view_y, zoom, cor);
        if (hit_rotation_handle(r, canvas_x, canvas_y, view_x, view_y, zoom, m)) {
            SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        } else if (hit_corner(cor, m) >= 0) {
            SetMouseCursor(MOUSE_CURSOR_RESIZE_ALL);
        } else if (point_in_image(r, dx, dy)) {
            SetMouseCursor(MOUSE_CURSOR_RESIZE_ALL);
        }
    }

    if (g_refs.selected != -1 && IsKeyPressed(KEY_ESCAPE) && g_refs.mode == REF_IDLE &&
        g_refs.renaming_idx < 0) {
        g_refs.selected = -1;
        return true;
    }

    if (g_refs.selected != -1 && g_refs.mode == REF_IDLE &&
        g_refs.renaming_idx < 0 &&
        (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE))) {
        RefImage *r = &g_refs.items[g_refs.selected];
        free_item(r);
        memmove(&g_refs.items[g_refs.selected],
                &g_refs.items[g_refs.selected + 1],
                (g_refs.count - g_refs.selected - 1) * sizeof(RefImage));
        g_refs.count--;
        g_refs.selected = -1;
        g_refs.dirty_after_release = 1;
        return true;
    }

    // Panel input (only when something is selected, and not mid-transform)
    if (g_refs.selected >= 0 && g_refs.mode == REF_IDLE) {
        RefImage *r = &g_refs.items[g_refs.selected];
        Rectangle pr = panel_rect(r, canvas_x, canvas_y, view_x, view_y, zoom,
                                  panel_w, panel_h);

        Rectangle slider = {pr.x + 12, pr.y + 16, pr.width - 24, 10};
        Rectangle lock_btn = {pr.x + pr.width - 58, pr.y + pr.height - 26, 22, 20};
        Rectangle del_btn  = {pr.x + pr.width - 30, pr.y + pr.height - 26, 22, 20};

        // Continue dragging slider (mode stays REF_IDLE; slider_dragging flag)
        if (g_refs.slider_dragging) {
            float t = (m.x - slider.x) / slider.width;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            r->opacity = t;
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                g_refs.slider_dragging = false;
                g_refs.dirty_after_release = 1;
            }
            return true;
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(m, pr)) {
            if (CheckCollisionPointRec(m, slider)) {
                g_refs.slider_dragging = true;
                float t = (m.x - slider.x) / slider.width;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                r->opacity = t;
                return true;
            }
            if (CheckCollisionPointRec(m, lock_btn)) {
                r->locked = !r->locked;
                g_refs.dirty_after_release = 1;
                return true;
            }
            if (CheckCollisionPointRec(m, del_btn)) {
                free_item(r);
                memmove(&g_refs.items[g_refs.selected],
                        &g_refs.items[g_refs.selected + 1],
                        (g_refs.count - g_refs.selected - 1) * sizeof(RefImage));
                g_refs.count--;
                g_refs.selected = -1;
                g_refs.dirty_after_release = 1;
                return true;
            }
            // Click inside panel but not on a control: still consume
            return true;
        }
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

    // Active drag (rotate)
    if (g_refs.mode == REF_ROTATING && g_refs.selected >= 0) {
        RefImage *r = &g_refs.items[g_refs.selected];
        float ang = atan2f(dy - r->y, dx - r->x);
        r->rotation = g_refs.rot_start_rot + (ang - g_refs.rot_start_angle);
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            g_refs.mode = REF_IDLE;
            g_refs.dirty_after_release = 1;
        }
        return true;
    }

    if (mouse_in_panel && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_refs.selected >= 0) {
            RefImage *r = &g_refs.items[g_refs.selected];

            if (hit_rotation_handle(r, canvas_x, canvas_y,
                                    view_x, view_y, zoom, m)) {
                g_refs.mode = REF_ROTATING;
                g_refs.rot_start_angle = atan2f(dy - r->y, dx - r->x);
                g_refs.rot_start_rot   = r->rotation;
                return true;
            }

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
            if (g_refs.items[i].locked) continue;
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

    // Rotation handle (circle) with connector line from top edge midpoint
    Vector2 rh = rotation_handle_screen(r, canvas_x, canvas_y, view_x, view_y, zoom);
    Vector2 top_mid = {(cor[0].x + cor[1].x) * 0.5f, (cor[0].y + cor[1].y) * 0.5f};
    DrawLineEx(top_mid, rh, 1.0f, (Color){0, 0, 0, 180});
    DrawCircleV(rh, 6.0f, WHITE);
    DrawCircleLinesV(rh, 6.0f, (Color){0, 0, 0, 200});
}

void refimage_draw_panel(int canvas_x, int canvas_y,
                        float view_x, float view_y, float zoom,
                        int panel_w, int panel_h) {
    if (g_refs.selected < 0 || g_refs.selected >= g_refs.count) return;
    RefImage *r = &g_refs.items[g_refs.selected];
    Rectangle pr = panel_rect(r, canvas_x, canvas_y, view_x, view_y, zoom,
                              panel_w, panel_h);

    DrawRectangleRec(pr, (Color){30, 30, 30, 230});
    DrawRectangleLinesEx(pr, 1.0f, (Color){80, 80, 80, 255});

    // Opacity slider
    Rectangle slider = {pr.x + 12, pr.y + 16, pr.width - 24, 10};
    DrawRectangleRec(slider, (Color){50, 50, 50, 255});
    Rectangle fill = slider;
    fill.width *= r->opacity;
    DrawRectangleRec(fill, (Color){180, 180, 200, 255});
    DrawRectangleLinesEx(slider, 1.0f, (Color){90, 90, 90, 255});

    // Lock + Delete buttons
    Rectangle lock_r = {pr.x + pr.width - 58, pr.y + pr.height - 26, 22, 20};
    Rectangle del = {pr.x + pr.width - 30, pr.y + pr.height - 26, 22, 20};
    DrawRectangleRec(lock_r,
                     r->locked ? (Color){200, 140, 40, 255} : (Color){60, 60, 60, 255});
    DrawRectangleLinesEx(lock_r, 1.0f, (Color){120, 120, 120, 255});
    DrawRectangleRec(del, (Color){120, 40, 40, 255});
    DrawRectangleLinesEx(del, 1.0f, (Color){200, 80, 80, 255});
}

void refimage_rename(int idx, const char *name) {
    if (idx < 0 || idx >= g_refs.count || !name) return;
    RefImage *r = &g_refs.items[idx];
    size_t n = strlen(name);
    if (n >= sizeof(r->name)) n = sizeof(r->name) - 1;
    memcpy(r->name, name, n);
    r->name[n] = '\0';
    g_refs.dirty_after_release = 1;
}

void refimage_toggle_locked(int idx) {
    if (idx < 0 || idx >= g_refs.count) return;
    g_refs.items[idx].locked = !g_refs.items[idx].locked;
    g_refs.dirty_after_release = 1;
}

void refimage_toggle_visible(int idx) {
    if (idx < 0 || idx >= g_refs.count) return;
    g_refs.items[idx].visible = !g_refs.items[idx].visible;
    g_refs.dirty_after_release = 1;
}

void refimage_select(int idx) {
    if (idx < -1 || idx >= g_refs.count) return;
    g_refs.selected = idx;
}

bool refimage_rename_active(void) { return g_refs.renaming_idx >= 0; }
int  refimage_rename_index(void) { return g_refs.renaming_idx; }

void refimage_rename_begin(int idx) {
    if (idx < 0 || idx >= g_refs.count) return;
    g_refs.renaming_idx = idx;
    size_t n = strlen(g_refs.items[idx].name);
    if (n >= sizeof(g_refs.rename_buf)) n = sizeof(g_refs.rename_buf) - 1;
    memcpy(g_refs.rename_buf, g_refs.items[idx].name, n);
    g_refs.rename_buf[n] = '\0';
    g_refs.rename_buf_len = (int)n;
}

void refimage_rename_commit(void) {
    if (g_refs.renaming_idx < 0) return;
    refimage_rename(g_refs.renaming_idx, g_refs.rename_buf);
    g_refs.renaming_idx = -1;
    g_refs.rename_buf[0] = '\0';
    g_refs.rename_buf_len = 0;
}

void refimage_rename_cancel(void) {
    g_refs.renaming_idx = -1;
    g_refs.rename_buf[0] = '\0';
    g_refs.rename_buf_len = 0;
}

char *refimage_rename_buffer(void) { return g_refs.rename_buf; }
int   refimage_rename_buffer_len(void) { return g_refs.rename_buf_len; }
void  refimage_rename_buffer_set_len(int len) {
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(g_refs.rename_buf)) len = (int)sizeof(g_refs.rename_buf) - 1;
    g_refs.rename_buf_len = len;
    g_refs.rename_buf[len] = '\0';
}

// (Move up/down is now handled in toolbar.c via z-swap)

#endif // REFIMAGE_IMPLEMENTATION
