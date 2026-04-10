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

// One continuous brush stroke (mouse-down → mouse-up).
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
    RenderTexture2D rt;
    int     width, height;
    Stroke *strokes;         // committed strokes
    int     stroke_count;
    int     stroke_capacity;
    Stroke  current;         // stroke being built right now
    bool    is_drawing;
    bool    dirty;           // unsaved changes
    int     view_x, view_y;  // pan offset (pixels)
} Canvas;

void canvas_init(Canvas *c);
void canvas_free(Canvas *c);

// Stroke lifecycle — call add_point every frame while mouse is held
void canvas_begin_stroke(Canvas *c, Color color, int radius, ToolType tool);
void canvas_add_point(Canvas *c, Vector2 p);   // renders segment to RT
void canvas_end_stroke(Canvas *c);
void canvas_undo(Canvas *c);
void canvas_clear(Canvas *c);

// Load strokes from DB (takes ownership of the array)
void canvas_load_strokes(Canvas *c, Stroke *strokes, int count);

void canvas_draw(const Canvas *c);
