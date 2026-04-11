#pragma once

#include <stdbool.h>
#include "sqlite3.h"
#include "canvas.h"   // for Stroke, Layer

typedef struct {
    int  id;
    char name[128];
    char updated_at[32];
    int  stroke_count;
} PaintingMeta;

bool db_open(sqlite3 **db);
void db_close(sqlite3 *db);

// Returns new painting_id (>0) on success, -1 on error.
int  db_save_painting(sqlite3 *db, const char *name,
                      const Layer *layers, int layer_count,
                      int width, int height);

// Caller owns the returned Layer array; free with db_free_layers().
bool db_load_painting(sqlite3 *db, int id,
                      Layer **out_layers, int *out_layer_count,
                      int *out_width, int *out_height);

bool db_list_paintings(sqlite3 *db, PaintingMeta **out_list, int *out_count);
void db_free_list(PaintingMeta *list);
void db_free_layers(Layer *layers, int count);

bool db_delete_painting(sqlite3 *db, int id);
