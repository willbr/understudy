#include "canvas.h"

#include <math.h>
#include <stdlib.h>

void canvas_init(Canvas *c) {
    c->width  = CANVAS_WIDTH;
    c->height = CANVAS_HEIGHT;
    c->image  = GenImageColor(c->width, c->height, WHITE);
    ImageFormat(&c->image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    c->texture    = LoadTextureFromImage(c->image);
    c->dirty      = false;
    c->is_drawing = false;
    c->last_mouse = (Vector2){0, 0};
}

void canvas_free(Canvas *c) {
    UnloadTexture(c->texture);
    UnloadImage(c->image);
}

void canvas_paint(Canvas *c, Vector2 from, Vector2 to, Color color, int radius) {
    float dx   = to.x - from.x;
    float dy   = to.y - from.y;
    float dist = sqrtf(dx * dx + dy * dy);
    float step = fmaxf(1.0f, (float)radius * 0.5f);
    int   steps = (int)(dist / step) + 1;

    for (int i = 0; i <= steps; i++) {
        float t  = (steps == 0) ? 0.0f : (float)i / (float)steps;
        int   cx = (int)(from.x + dx * t);
        int   cy = (int)(from.y + dy * t);

        // Clamp to avoid out-of-bounds writes
        if (cx < radius)          cx = radius;
        if (cx > c->width - radius - 1)  cx = c->width - radius - 1;
        if (cy < radius)          cy = radius;
        if (cy > c->height - radius - 1) cy = c->height - radius - 1;

        ImageDrawCircle(&c->image, cx, cy, radius, color);
    }

    UpdateTexture(c->texture, c->image.data);
    c->dirty = true;
}

void canvas_clear(Canvas *c) {
    ImageClearBackground(&c->image, WHITE);
    UpdateTexture(c->texture, c->image.data);
    c->dirty = false;
}

void canvas_draw(const Canvas *c) {
    Rectangle src  = {0, 0, (float)c->width, (float)c->height};
    Rectangle dest = {CANVAS_X, CANVAS_Y, (float)c->width, (float)c->height};
    DrawTexturePro(c->texture, src, dest, (Vector2){0, 0}, 0.0f, WHITE);
}

unsigned char *canvas_export_png(Canvas *c, int *out_size) {
    return ExportImageToMemory(c->image, ".png", out_size);
}

bool canvas_import_png(Canvas *c, const unsigned char *data, int size) {
    Image loaded = LoadImageFromMemory(".png", data, size);
    if (loaded.data == NULL) return false;

    ImageFormat(&loaded, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    UnloadImage(c->image);
    UnloadTexture(c->texture);

    c->image   = loaded;
    c->width   = loaded.width;
    c->height  = loaded.height;
    c->texture = LoadTextureFromImage(c->image);
    c->dirty   = false;
    return true;
}
