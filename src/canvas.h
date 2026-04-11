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
} Layer;

typedef struct Canvas {
    RenderTexture2D rt;
    RenderTexture2D minimap_rt;  // thumbnail of full document at MINIMAP_SIZE scale
    Texture2D       paper_tex;   // tileable paper grain texture
    int     width, height;

    Layer   layers[MAX_LAYERS];
    int     layer_count;
    int     active_layer;        // index into layers[]

    Stroke  current;         // stroke being built right now
    bool    is_drawing;
    bool    dirty;           // unsaved changes
    float   view_x, view_y;  // pan offset (screen pixels)
    float   zoom;            // scale factor: 1.0 = 100%
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

// Export the full document as a PNG file at 1:1 resolution
bool canvas_export_png(Canvas *c, const char *path);

// Re-render all strokes at the current zoom/pan — call after any view change
void canvas_redraw_for_view(Canvas *c);

// Recreate the RT at the new panel dimensions and redraw — call on window resize
void canvas_resize(Canvas *c, int panel_w, int panel_h);

// Draw the minimap overlay — call inside BeginDrawing; alpha 0..1 for fade
void canvas_draw_minimap(const Canvas *c, float alpha, int x_offset);

void canvas_draw(const Canvas *c, int x_offset);

// Layer management
int  canvas_add_layer(Canvas *c);                          // returns index or -1
void canvas_delete_layer(Canvas *c, int idx);
void canvas_set_active_layer(Canvas *c, int idx);
void canvas_toggle_layer_visible(Canvas *c, int idx);
void canvas_rename_layer(Canvas *c, int idx, const char *name);
void canvas_move_layer(Canvas *c, int from, int to);
