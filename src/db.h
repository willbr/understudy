#pragma once

#include <stdbool.h>
#include "sqlite3.h"
#include "canvas.h"   // for Stroke

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
                      const Stroke *strokes, int count,
                      int width, int height);

// Caller owns the returned Stroke array; free with db_free_strokes().
bool db_load_painting(sqlite3 *db, int id,
                      Stroke **out_strokes, int *out_count,
                      int *out_width, int *out_height);

bool db_list_paintings(sqlite3 *db, PaintingMeta **out_list, int *out_count);
void db_free_list(PaintingMeta *list);
void db_free_strokes(Stroke *strokes, int count);

bool db_delete_painting(sqlite3 *db, int id);
