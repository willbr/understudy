#pragma once

#include <stdbool.h>
#include "sqlite3.h"
#include "canvas.h"   // for Stroke, Layer

typedef struct RefImage RefImage;   // forward-declare (defined in refimage.h)

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

// Autosave: overwrites the autosave for the current session (tracked by ID)
void db_autosave(sqlite3 *db, int *autosave_id,
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
bool db_rename_painting(sqlite3 *db, int id, const char *new_name);

// Wipe & rewrite ref_images for this painting. Returns 0 on success.
int  db_save_ref_images(sqlite3 *db, int painting_id,
                        const RefImage *images, int count);

// Loads all ref images for painting_id into a freshly allocated array.
// Each entry's texture is uploaded and png_bytes is malloc'd.
// Returns true on success (even if 0 rows). Caller owns *out.
bool db_load_ref_images(sqlite3 *db, int painting_id,
                        RefImage **out, int *out_count);

// Free a loaded array: unloads textures, frees png bytes, frees array.
void db_free_ref_images(RefImage *images, int count);
