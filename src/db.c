#include "db.h"

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
    "  visible     INTEGER NOT NULL DEFAULT 1"
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
    ");";

// ── Open/close ────────────────────────────────────────────────────────────────

#define SCHEMA_VERSION 3

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
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/claude-paint", home);
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
        "INSERT INTO layers (painting_id, layer_idx, name, visible) VALUES (?, ?, ?, ?);";
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
        sqlite3_bind_int (layer_stmt, 1, painting_id);
        sqlite3_bind_int (layer_stmt, 2, li);
        sqlite3_bind_text(layer_stmt, 3, l->name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (layer_stmt, 4, l->visible ? 1 : 0);
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
                 int width, int height) {
    // Delete previous autosave for THIS canvas session
    if (*autosave_id > 0) {
        sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
        char sql[128];
        snprintf(sql, sizeof(sql), "DELETE FROM paintings WHERE id = %d;", *autosave_id);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }

    // Save and track the new ID
    *autosave_id = db_save_painting(db, "_autosave", layers, layer_count, width, height);
}

// ── Load ──────────────────────────────────────────────────────────────────────

bool db_load_painting(sqlite3 *db, int id,
                      Layer **out_layers, int *out_layer_count,
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
        "SELECT id, name, visible FROM layers WHERE painting_id = ? ORDER BY layer_idx ASC;";
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
        l->visible = sqlite3_column_int(stmt, 2) != 0;
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
