#include "db.h"
#include "refimage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ── Schema ────────────────────────────────────────────────────────────────────

static const char *SCHEMA_SQL =
    "PRAGMA foreign_keys = ON;"

    "CREATE TABLE IF NOT EXISTS paintings ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name       TEXT    NOT NULL,"
    "  created_at TEXT    NOT NULL DEFAULT (datetime('now')),"
    "  updated_at TEXT    NOT NULL DEFAULT (datetime('now')),"
    "  width      INTEGER NOT NULL,"
    "  height     INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS layers ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  painting_id INTEGER NOT NULL REFERENCES paintings(id) ON DELETE CASCADE,"
    "  layer_idx   INTEGER NOT NULL,"
    "  name        TEXT    NOT NULL,"
    "  visible     INTEGER NOT NULL DEFAULT 1,"
    "  z           REAL    NOT NULL DEFAULT 0,"
    "  pan_x       REAL    NOT NULL DEFAULT 0,"
    "  pan_y       REAL    NOT NULL DEFAULT 0,"
    "  opacity     REAL    NOT NULL DEFAULT 1"
    ");"

    "CREATE TABLE IF NOT EXISTS strokes ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  painting_id INTEGER NOT NULL REFERENCES paintings(id) ON DELETE CASCADE,"
    "  layer_id    INTEGER NOT NULL REFERENCES layers(id) ON DELETE CASCADE,"
    "  stroke_idx  INTEGER NOT NULL,"
    "  color_r     INTEGER NOT NULL,"
    "  color_g     INTEGER NOT NULL,"
    "  color_b     INTEGER NOT NULL,"
    "  color_a     INTEGER NOT NULL,"
    "  radius      INTEGER NOT NULL,"
    "  tool        INTEGER NOT NULL,"
    "  points      BLOB    NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS ref_images ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  painting_id   INTEGER NOT NULL REFERENCES paintings(id) ON DELETE CASCADE,"
    "  image_idx     INTEGER NOT NULL,"
    "  x             REAL    NOT NULL,"
    "  y             REAL    NOT NULL,"
    "  scale         REAL    NOT NULL,"
    "  rotation      REAL    NOT NULL,"
    "  opacity       REAL    NOT NULL,"
    "  z             REAL    NOT NULL,"
    "  name          TEXT    NOT NULL DEFAULT '',"
    "  locked        INTEGER NOT NULL DEFAULT 0,"
    "  visible       INTEGER NOT NULL DEFAULT 1,"
    "  png_blob      BLOB    NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_ref_images_painting "
    "ON ref_images(painting_id, image_idx);";

// ── Open/close ────────────────────────────────────────────────────────────────

#define SCHEMA_VERSION 8

static bool migrate(sqlite3 *db) {
    sqlite3_stmt *stmt;
    int version = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    if (version == SCHEMA_VERSION) return true;

    if (version > 0)
        fprintf(stderr, "db: schema version %d -> %d, rebuilding (old data dropped)\n",
                version, SCHEMA_VERSION);

    sqlite3_exec(db, "DROP TABLE IF EXISTS ref_images;", NULL, NULL, NULL);
    sqlite3_exec(db, "DROP TABLE IF EXISTS strokes;",   NULL, NULL, NULL);
    sqlite3_exec(db, "DROP TABLE IF EXISTS layers;",    NULL, NULL, NULL);
    sqlite3_exec(db, "DROP TABLE IF EXISTS paintings;", NULL, NULL, NULL);

    char *err = NULL;
    if (sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "db migrate schema: %s\n", err);
        sqlite3_free(err);
        return false;
    }

    char set_ver[64];
    snprintf(set_ver, sizeof(set_ver), "PRAGMA user_version = %d;", SCHEMA_VERSION);
    sqlite3_exec(db, set_ver, NULL, NULL, NULL);
    return true;
}

bool db_open(sqlite3 **db) {
    const char *home = getenv("HOME");
    if (!home) return false;

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/Understudy", home);
    mkdir(dir, 0755);

    char path[600];
    snprintf(path, sizeof(path), "%s/paintings.db", dir);

    if (sqlite3_open(path, db) != SQLITE_OK) {
        fprintf(stderr, "db_open: %s\n", sqlite3_errmsg(*db));
        return false;
    }

    sqlite3_exec(*db, "PRAGMA journal_mode=WAL;",   NULL, NULL, NULL);
    sqlite3_exec(*db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    return migrate(*db);
}

void db_close(sqlite3 *db) {
    sqlite3_close(db);
}

// ── Save ──────────────────────────────────────────────────────────────────────

int db_save_painting(sqlite3 *db, const char *name,
                     const Layer *layers, int layer_count,
                     const RefImage *refs, int ref_count,
                     int width, int height) {
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

    // Insert painting row
    sqlite3_stmt *stmt;
    const char *ins_painting =
        "INSERT INTO paintings (name, width, height) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(db, ins_painting, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_save paintings prepare: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, name,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, width);
    sqlite3_bind_int (stmt, 3, height);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        fprintf(stderr, "db_save paintings step: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }
    sqlite3_finalize(stmt);
    int painting_id = (int)sqlite3_last_insert_rowid(db);

    // Insert layers and their strokes
    const char *ins_layer =
        "INSERT INTO layers (painting_id, layer_idx, name, visible, z, pan_x, pan_y, opacity) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    const char *ins_stroke =
        "INSERT INTO strokes "
        "(painting_id, layer_id, stroke_idx, color_r, color_g, color_b, color_a, radius, tool, points)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *layer_stmt, *stroke_stmt;
    if (sqlite3_prepare_v2(db, ins_layer, -1, &layer_stmt, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }
    if (sqlite3_prepare_v2(db, ins_stroke, -1, &stroke_stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(layer_stmt);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }

    for (int li = 0; li < layer_count; li++) {
        const Layer *l = &layers[li];

        sqlite3_reset(layer_stmt);
        sqlite3_bind_int   (layer_stmt, 1, painting_id);
        sqlite3_bind_int   (layer_stmt, 2, li);
        sqlite3_bind_text  (layer_stmt, 3, l->name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (layer_stmt, 4, l->visible ? 1 : 0);
        sqlite3_bind_double(layer_stmt, 5, l->z);
        sqlite3_bind_double(layer_stmt, 6, l->pan_x);
        sqlite3_bind_double(layer_stmt, 7, l->pan_y);
        sqlite3_bind_double(layer_stmt, 8, l->opacity);
        if (sqlite3_step(layer_stmt) != SQLITE_DONE) {
            fprintf(stderr, "db_save layer %d: %s\n", li, sqlite3_errmsg(db));
            goto fail;
        }
        int layer_id = (int)sqlite3_last_insert_rowid(db);

        for (int si = 0; si < l->stroke_count; si++) {
            const Stroke *s = &l->strokes[si];
            int blob_size = s->count * (int)sizeof(Vector2);

            sqlite3_reset(stroke_stmt);
            sqlite3_bind_int (stroke_stmt, 1, painting_id);
            sqlite3_bind_int (stroke_stmt, 2, layer_id);
            sqlite3_bind_int (stroke_stmt, 3, si);
            sqlite3_bind_int (stroke_stmt, 4, s->color.r);
            sqlite3_bind_int (stroke_stmt, 5, s->color.g);
            sqlite3_bind_int (stroke_stmt, 6, s->color.b);
            sqlite3_bind_int (stroke_stmt, 7, s->color.a);
            sqlite3_bind_int (stroke_stmt, 8, s->radius);
            sqlite3_bind_int (stroke_stmt, 9, (int)s->tool);
            sqlite3_bind_blob(stroke_stmt, 10, s->points, blob_size, SQLITE_TRANSIENT);

            if (sqlite3_step(stroke_stmt) != SQLITE_DONE) {
                fprintf(stderr, "db_save stroke %d/%d: %s\n", li, si, sqlite3_errmsg(db));
                goto fail;
            }
        }
    }

    if (ref_count > 0 && refs) {
        const char *ins_ref =
            "INSERT INTO ref_images "
            "(painting_id, image_idx, x, y, scale, rotation, opacity, z, "
            "name, locked, visible, png_blob) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt *ref_stmt;
        if (sqlite3_prepare_v2(db, ins_ref, -1, &ref_stmt, NULL) != SQLITE_OK) {
            goto fail;
        }
        for (int ri = 0; ri < ref_count; ri++) {
            const RefImage *r = &refs[ri];
            sqlite3_reset(ref_stmt);
            sqlite3_bind_int   (ref_stmt, 1, painting_id);
            sqlite3_bind_int   (ref_stmt, 2, ri);
            sqlite3_bind_double(ref_stmt, 3, r->x);
            sqlite3_bind_double(ref_stmt, 4, r->y);
            sqlite3_bind_double(ref_stmt, 5, r->scale);
            sqlite3_bind_double(ref_stmt, 6, r->rotation);
            sqlite3_bind_double(ref_stmt, 7, r->opacity);
            sqlite3_bind_double(ref_stmt, 8, r->z);
            sqlite3_bind_text  (ref_stmt, 9, r->name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int   (ref_stmt, 10, r->locked ? 1 : 0);
            sqlite3_bind_int   (ref_stmt, 11, r->visible ? 1 : 0);
            sqlite3_bind_blob  (ref_stmt, 12, r->png_bytes, r->png_len, SQLITE_TRANSIENT);
            if (sqlite3_step(ref_stmt) != SQLITE_DONE) {
                sqlite3_finalize(ref_stmt);
                goto fail;
            }
        }
        sqlite3_finalize(ref_stmt);
    }

    sqlite3_finalize(layer_stmt);
    sqlite3_finalize(stroke_stmt);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    return painting_id;

fail:
    sqlite3_finalize(layer_stmt);
    sqlite3_finalize(stroke_stmt);
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
}

// ── Autosave ─────────────────────────────────────────────────────────────────

void db_autosave(sqlite3 *db, int *autosave_id,
                 const Layer *layers, int layer_count,
                 const RefImage *refs, int ref_count,
                 int width, int height) {
    if (*autosave_id > 0) {
        sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
        char sql[128];
        snprintf(sql, sizeof(sql), "DELETE FROM paintings WHERE id = %d;", *autosave_id);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    *autosave_id = db_save_painting(db, "_autosave", layers, layer_count,
                                    refs, ref_count, width, height);
}

// ── Load ──────────────────────────────────────────────────────────────────────

bool db_load_painting(sqlite3 *db, int id,
                      Layer **out_layers, int *out_layer_count,
                      RefImage **out_refs, int *out_ref_count,
                      int *out_width, int *out_height) {
    // Fetch painting dimensions
    sqlite3_stmt *stmt;
    const char *sel = "SELECT width, height FROM paintings WHERE id = ?;";
    if (sqlite3_prepare_v2(db, sel, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return false; }
    *out_width  = sqlite3_column_int(stmt, 0);
    *out_height = sqlite3_column_int(stmt, 1);
    sqlite3_finalize(stmt);

    // Fetch layers
    const char *sel_layers =
        "SELECT id, name, visible, z, pan_x, pan_y, opacity FROM layers WHERE painting_id = ? ORDER BY layer_idx ASC;";
    if (sqlite3_prepare_v2(db, sel_layers, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);

    int layer_cap = 8, lcount = 0;
    Layer *layers = calloc(layer_cap, sizeof(Layer));
    if (!layers) { sqlite3_finalize(stmt); return false; }

    // Collect layer metadata
    typedef struct { int db_id; } LayerInfo;
    LayerInfo *infos = calloc(layer_cap, sizeof(LayerInfo));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (lcount >= layer_cap) {
            layer_cap *= 2;
            Layer *tmp = realloc(layers, layer_cap * sizeof(Layer));
            LayerInfo *ti = realloc(infos, layer_cap * sizeof(LayerInfo));
            if (!tmp || !ti) break;
            layers = tmp;
            infos  = ti;
        }
        Layer *l = &layers[lcount];
        memset(l, 0, sizeof(*l));
        infos[lcount].db_id = sqlite3_column_int(stmt, 0);
        const char *n = (const char *)sqlite3_column_text(stmt, 1);
        snprintf(l->name, LAYER_NAME_LEN, "%s", n ? n : "Layer");
        l->visible  = sqlite3_column_int(stmt, 2) != 0;
        l->z        = (float)sqlite3_column_double(stmt, 3);
        l->pan_x    = (float)sqlite3_column_double(stmt, 4);
        l->pan_y    = (float)sqlite3_column_double(stmt, 5);
        l->opacity  = (float)sqlite3_column_double(stmt, 6);
        if (l->opacity <= 0.0f && sqlite3_column_type(stmt, 6) == SQLITE_NULL)
            l->opacity = 1.0f;  // default for rows without opacity column
        lcount++;
    }
    sqlite3_finalize(stmt);

    // Fetch strokes for each layer
    const char *sel_strokes =
        "SELECT color_r, color_g, color_b, color_a, radius, tool, points "
        "FROM strokes WHERE layer_id = ? ORDER BY stroke_idx ASC;";

    for (int li = 0; li < lcount; li++) {
        if (sqlite3_prepare_v2(db, sel_strokes, -1, &stmt, NULL) != SQLITE_OK) continue;
        sqlite3_bind_int(stmt, 1, infos[li].db_id);

        Layer *l = &layers[li];
        int scap = 16;
        l->strokes = malloc(scap * sizeof(Stroke));
        l->stroke_count = 0;
        l->stroke_capacity = scap;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (l->stroke_count >= l->stroke_capacity) {
                l->stroke_capacity *= 2;
                Stroke *tmp = realloc(l->strokes, l->stroke_capacity * sizeof(Stroke));
                if (!tmp) break;
                l->strokes = tmp;
            }
            Stroke *s = &l->strokes[l->stroke_count++];
            s->color.r = (unsigned char)sqlite3_column_int(stmt, 0);
            s->color.g = (unsigned char)sqlite3_column_int(stmt, 1);
            s->color.b = (unsigned char)sqlite3_column_int(stmt, 2);
            s->color.a = (unsigned char)sqlite3_column_int(stmt, 3);
            s->radius  = sqlite3_column_int(stmt, 4);
            s->tool    = (ToolType)sqlite3_column_int(stmt, 5);

            int blob_size = sqlite3_column_bytes(stmt, 6);
            s->count    = blob_size / (int)sizeof(Vector2);
            s->capacity = s->count;
            s->points   = malloc(blob_size);
            if (s->points)
                memcpy(s->points, sqlite3_column_blob(stmt, 6), blob_size);
        }
        sqlite3_finalize(stmt);
    }

    free(infos);
    *out_layers      = layers;
    *out_layer_count = lcount;

    if (out_refs && out_ref_count) {
        if (!db_load_ref_images(db, id, out_refs, out_ref_count)) {
            *out_refs = NULL;
            *out_ref_count = 0;
        }
    }

    return true;
}

// ── List ──────────────────────────────────────────────────────────────────────

bool db_list_paintings(sqlite3 *db, PaintingMeta **out_list, int *out_count) {
    const char *sql =
        "SELECT p.id, p.name, p.updated_at, COUNT(s.id) "
        "FROM paintings p LEFT JOIN strokes s ON s.painting_id = p.id "
        "GROUP BY p.id ORDER BY p.updated_at DESC;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_list prepare: %s\n", sqlite3_errmsg(db));
        return false;
    }

    int capacity = 16, count = 0;
    PaintingMeta *list = malloc(capacity * sizeof(PaintingMeta));
    if (!list) { sqlite3_finalize(stmt); return false; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            PaintingMeta *tmp = realloc(list, capacity * sizeof(PaintingMeta));
            if (!tmp) break;
            list = tmp;
        }
        PaintingMeta *m = &list[count++];
        m->id           = sqlite3_column_int(stmt, 0);
        m->stroke_count = sqlite3_column_int(stmt, 3);
        strncpy(m->name,       (const char *)sqlite3_column_text(stmt, 1), sizeof(m->name) - 1);
        strncpy(m->updated_at, (const char *)sqlite3_column_text(stmt, 2), sizeof(m->updated_at) - 1);
        m->name[sizeof(m->name) - 1]             = '\0';
        m->updated_at[sizeof(m->updated_at) - 1] = '\0';
    }
    sqlite3_finalize(stmt);
    *out_list  = list;
    *out_count = count;
    return true;
}

void db_free_list(PaintingMeta *list) { free(list); }

void db_free_layers(Layer *layers, int count) {
    for (int i = 0; i < count; i++) {
        for (int s = 0; s < layers[i].stroke_count; s++)
            free(layers[i].strokes[s].points);
        free(layers[i].strokes);
    }
    free(layers);
}

// ── Delete ────────────────────────────────────────────────────────────────────

bool db_delete_painting(sqlite3 *db, int id) {
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "DELETE FROM paintings WHERE id = ?;",
                           -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool db_rename_painting(sqlite3 *db, int id, const char *new_name) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "UPDATE paintings SET name = ?, updated_at = datetime('now') WHERE id = ?;",
                           -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

// ── Reference images ─────────────────────────────────────────────────────────

int db_save_ref_images(sqlite3 *db, int painting_id,
                       const RefImage *images, int count) {
    sqlite3_stmt *del_stmt, *ins_stmt;
    const char *del = "DELETE FROM ref_images WHERE painting_id = ?;";
    const char *ins =
        "INSERT INTO ref_images "
        "(painting_id, image_idx, x, y, scale, rotation, opacity, z, "
        "name, locked, visible, png_blob) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    if (sqlite3_prepare_v2(db, del, -1, &del_stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(del_stmt, 1, painting_id);
    if (sqlite3_step(del_stmt) != SQLITE_DONE) {
        sqlite3_finalize(del_stmt);
        return -1;
    }
    sqlite3_finalize(del_stmt);

    if (sqlite3_prepare_v2(db, ins, -1, &ins_stmt, NULL) != SQLITE_OK) return -1;

    for (int i = 0; i < count; i++) {
        const RefImage *r = &images[i];
        sqlite3_reset(ins_stmt);
        sqlite3_bind_int   (ins_stmt, 1, painting_id);
        sqlite3_bind_int   (ins_stmt, 2, i);
        sqlite3_bind_double(ins_stmt, 3, r->x);
        sqlite3_bind_double(ins_stmt, 4, r->y);
        sqlite3_bind_double(ins_stmt, 5, r->scale);
        sqlite3_bind_double(ins_stmt, 6, r->rotation);
        sqlite3_bind_double(ins_stmt, 7, r->opacity);
        sqlite3_bind_double(ins_stmt, 8, r->z);
        sqlite3_bind_text  (ins_stmt, 9, r->name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (ins_stmt, 10, r->locked ? 1 : 0);
        sqlite3_bind_int   (ins_stmt, 11, r->visible ? 1 : 0);
        sqlite3_bind_blob  (ins_stmt, 12, r->png_bytes, r->png_len, SQLITE_TRANSIENT);

        if (sqlite3_step(ins_stmt) != SQLITE_DONE) {
            fprintf(stderr, "db_save_ref_images %d: %s\n", i, sqlite3_errmsg(db));
            sqlite3_finalize(ins_stmt);
            return -1;
        }
    }
    sqlite3_finalize(ins_stmt);
    return 0;
}

bool db_load_ref_images(sqlite3 *db, int painting_id,
                        RefImage **out, int *out_count) {
    *out = NULL;
    *out_count = 0;

    sqlite3_stmt *stmt;
    const char *sel =
        "SELECT id, x, y, scale, rotation, opacity, z, "
        "name, locked, visible, png_blob "
        "FROM ref_images WHERE painting_id = ? ORDER BY image_idx ASC;";
    if (sqlite3_prepare_v2(db, sel, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, painting_id);

    int cap = 0, cnt = 0;
    RefImage *arr = NULL;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cnt == cap) {
            cap = cap ? cap * 2 : 4;
            arr = realloc(arr, cap * sizeof(RefImage));
        }
        RefImage *r = &arr[cnt];
        r->db_id    = sqlite3_column_int   (stmt, 0);
        r->x        = (float)sqlite3_column_double(stmt, 1);
        r->y        = (float)sqlite3_column_double(stmt, 2);
        r->scale    = (float)sqlite3_column_double(stmt, 3);
        r->rotation = (float)sqlite3_column_double(stmt, 4);
        r->opacity  = (float)sqlite3_column_double(stmt, 5);
        r->z        = (float)sqlite3_column_double(stmt, 6);

        const char *nm = (const char *)sqlite3_column_text(stmt, 7);
        if (nm) {
            size_t n = strlen(nm);
            if (n >= sizeof(r->name)) n = sizeof(r->name) - 1;
            memcpy(r->name, nm, n);
            r->name[n] = '\0';
        } else {
            r->name[0] = '\0';
        }
        r->locked  = sqlite3_column_int(stmt, 8) != 0;
        r->visible = sqlite3_column_int(stmt, 9) != 0;

        const void *blob = sqlite3_column_blob(stmt, 10);
        int blob_len     = sqlite3_column_bytes(stmt, 10);
        r->png_bytes = malloc(blob_len);
        memcpy(r->png_bytes, blob, blob_len);
        r->png_len   = blob_len;

        Image img = LoadImageFromMemory(".png", r->png_bytes, blob_len);
        if (!img.data) {
            img = LoadImageFromMemory(".jpg", r->png_bytes, blob_len);
        }
        if (!img.data) {
            fprintf(stderr, "db_load_ref_images: failed to decode image %d\n", cnt);
            free(r->png_bytes);
            continue;
        }
        r->src_w = img.width;
        r->src_h = img.height;
        r->tex = LoadTextureFromImage(img);
        UnloadImage(img);

        cnt++;
    }
    sqlite3_finalize(stmt);

    *out = arr;
    *out_count = cnt;
    return true;
}

void db_free_ref_images(RefImage *images, int count) {
    if (!images) return;
    for (int i = 0; i < count; i++) {
        if (images[i].tex.id) UnloadTexture(images[i].tex);
        free(images[i].png_bytes);
    }
    free(images);
}
