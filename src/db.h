#pragma once

#include <stdbool.h>
#include "sqlite3.h"

typedef struct {
    int  id;
    char name[128];
    char created_at[32];
    int  width;
    int  height;
} PaintingMeta;

bool db_open(sqlite3 **db);
void db_close(sqlite3 *db);

bool db_save_painting(sqlite3 *db, const char *name,
                      const unsigned char *blob, int blob_size,
                      int width, int height);

bool db_load_painting(sqlite3 *db, int id,
                      unsigned char **out_blob, int *out_blob_size,
                      int *out_width, int *out_height);

bool db_list_paintings(sqlite3 *db, PaintingMeta **out_list, int *out_count);
void db_free_list(PaintingMeta *list);

bool db_delete_painting(sqlite3 *db, int id);
