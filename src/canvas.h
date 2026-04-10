#pragma once

#include <stdbool.h>
#include "raylib.h"

#define CANVAS_X      220
#define CANVAS_Y      0
#define CANVAS_WIDTH  1060
#define CANVAS_HEIGHT 800

typedef struct {
    Image     image;
    Texture2D texture;
    int       width;
    int       height;
    bool      dirty;
    Vector2   last_mouse;
    bool      is_drawing;
} Canvas;

void  canvas_init(Canvas *c);
void  canvas_free(Canvas *c);
void  canvas_paint(Canvas *c, Vector2 from, Vector2 to, Color color, int radius);
void  canvas_clear(Canvas *c);
void  canvas_draw(const Canvas *c);

// PNG blob I/O for SQLite storage
unsigned char *canvas_export_png(Canvas *c, int *out_size);
bool           canvas_import_png(Canvas *c, const unsigned char *data, int size);
