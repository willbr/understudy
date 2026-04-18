#pragma once

#include <stdbool.h>
#include "raylib.h"
#include "tools.h"

#define CANVAS_X       220
#define CANVAS_Y       0
#define CANVAS_WIDTH   1060   // panel display area
#define CANVAS_HEIGHT  800    // panel display area
#define CANVAS_DOC_W   4096   // document / RenderTexture size
#define CANVAS_DOC_H   4096
#define MINIMAP_SIZE   160    // minimap thumbnail (square, matches square doc)

#define MAX_LAYERS      32
#define LAYER_NAME_LEN  32

// One continuous brush stroke (mouse-down -> mouse-up).
// Points are raw mouse samples; rendering interpolates between them.
typedef struct {
    Vector2 *points;
    int      count;
    int      capacity;
    Color    color;
    int      radius;
    ToolType tool;
} Stroke;

typedef struct {
    char     name[LAYER_NAME_LEN];
    Stroke  *strokes;
    int      stroke_count;
    int      stroke_capacity;
    bool     visible;
    float    z;              // unified z-order; higher = rendered on top
    float    pan_x;          // per-layer offset in document space
    float    pan_y;
} Layer;

typedef struct Canvas {
    RenderTexture2D rt;
    RenderTexture2D strokes_rt;    // strokes-only render (for ink shader compositing)
    RenderTexture2D committed_rt;  // cached committed strokes (avoids full re-render per frame)
    RenderTexture2D minimap_rt;    // thumbnail of full document at MINIMAP_SIZE scale
    Texture2D       paper_tex;   // fallback paper texture (used for minimap)
    RenderTexture2D paper_rt;     // paper rendered at current view transform
    Shader          paper_shader; // paper.fs — used for paper_rt
    Shader          ink_shader;  // composites strokes with procedural paper
    int     width, height;

    Layer   layers[MAX_LAYERS];
    int     layer_count;
    int     active_layer;        // index into layers[]

    Stroke  current;         // stroke being built right now
    bool    is_drawing;
    bool    dirty;           // unsaved changes
    float   view_x, view_y;  // pan offset (screen pixels)
    float   zoom;            // scale factor: 1.0 = 100%
    float   next_z;          // monotonic counter for z assignment
} Canvas;

void canvas_init(Canvas *c);
void canvas_free(Canvas *c);

// Stroke lifecycle — call add_point every frame while mouse is held
void canvas_begin_stroke(Canvas *c, Color color, int radius, ToolType tool);
void canvas_add_point(Canvas *c, Vector2 p);   // renders segment to RT
void canvas_end_stroke(Canvas *c);
void canvas_undo(Canvas *c);
void canvas_clear(Canvas *c);

// Load layers from DB (takes ownership of the layers array)
void canvas_load_layers(Canvas *c, Layer *layers, int layer_count);

// Export the full document as a PNG file at the given scale (1=native, 2=2x, etc.)
bool canvas_export_png(Canvas *c, const char *path, int scale);

// Crop the document to the given rectangle (document-space coordinates)
void canvas_crop(Canvas *c, int x, int y, int w, int h);

// Resize the document, scaling all strokes proportionally
void canvas_resize_doc(Canvas *c, int new_w, int new_h);

// Re-render all strokes at the current zoom/pan — call after any view change
void canvas_redraw_for_view(Canvas *c);

// Update the minimap thumbnail — call after pan offsets change
void canvas_update_minimap(Canvas *c);

// Recreate the RT at the new panel dimensions and redraw — call on window resize
void canvas_resize(Canvas *c, int panel_w, int panel_h);

// Draw the minimap overlay — call inside BeginDrawing; alpha 0..1 for fade
void canvas_draw_minimap(const Canvas *c, float alpha, int x_offset);

// Background fill (dark gray) for area outside document
void canvas_draw_dark_bg(const Canvas *c, int x_offset);

// Paper layer (below strokes, above dark bg)
void canvas_draw_paper(const Canvas *c, int x_offset);

// Strokes layer (transparent where no ink)
void canvas_draw_strokes(const Canvas *c, int x_offset);

// Render just one layer's strokes through the ink shader and blit to screen.
// Does nothing if the layer is invisible or has no strokes.
void canvas_draw_layer(Canvas *c, int li, int x_offset);

// Document border (drawn on top of everything canvas-related)
void canvas_draw_border(const Canvas *c, int x_offset);

// Bump next_z so that z+1 <= next_z (used after loading refs from DB)
void canvas_bump_next_z_to(Canvas *c, float z);

// Low-level rendering (used by line tool preview)
void render_stroke_transformed(const Stroke *s, float vx, float vy, float zoom);
void composite_ink(Canvas *c);

// Layer management
int  canvas_add_layer(Canvas *c);                          // returns index or -1
void canvas_delete_layer(Canvas *c, int idx);
void canvas_set_active_layer(Canvas *c, int idx);
void canvas_toggle_layer_visible(Canvas *c, int idx);
void canvas_rename_layer(Canvas *c, int idx, const char *name);
void canvas_move_layer(Canvas *c, int from, int to);
