#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ── Schema ────────────────────────────────────────────────────────────────────
//
// paintings  — one row per saved painting
// strokes    — one row per stroke, points packed as raw Vector2 (float[2]) BLOB
//
// ON DELETE CASCADE removes strokes automatically when a painting is deleted.

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

    "CREATE TABLE IF NOT EXISTS strokes ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  painting_id INTEGER NOT NULL REFERENCES paintings(id) ON DELETE CASCADE,"
    "  stroke_idx  INTEGER NOT NULL,"
    "  color_r     INTEGER NOT NULL,"
    "  color_g     INTEGER NOT NULL,"
    "  color_b     INTEGER NOT NULL,"
    "  color_a     INTEGER NOT NULL,"
    "  radius      INTEGER NOT NULL,"
    "  tool        INTEGER NOT NULL,"
    "  points      BLOB    NOT NULL"   // packed float pairs: [x0,y0, x1,y1, ...]
    ");";

// ── Open/close ────────────────────────────────────────────────────────────────

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

    char *err = NULL;
    if (sqlite3_exec(*db, SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "db_open schema: %s\n", err);
        sqlite3_free(err);
        return false;
    }
    return true;
}

void db_close(sqlite3 *db) {
    sqlite3_close(db);
}

// ── Save ──────────────────────────────────────────────────────────────────────

int db_save_painting(sqlite3 *db, const char *name,
                     const Stroke *strokes, int count,
                     int width, int height) {
    // Wrap everything in a transaction for atomicity
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

    // Insert paintings row
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

    // Insert each stroke
    const char *ins_stroke =
        "INSERT INTO strokes "
        "(painting_id, stroke_idx, color_r, color_g, color_b, color_a, radius, tool, points)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, ins_stroke, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_save strokes prepare: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        const Stroke *s = &strokes[i];
        int blob_size = s->count * (int)sizeof(Vector2);

        sqlite3_reset(stmt);
        sqlite3_bind_int (stmt, 1, painting_id);
        sqlite3_bind_int (stmt, 2, i);
        sqlite3_bind_int (stmt, 3, s->color.r);
        sqlite3_bind_int (stmt, 4, s->color.g);
        sqlite3_bind_int (stmt, 5, s->color.b);
        sqlite3_bind_int (stmt, 6, s->color.a);
        sqlite3_bind_int (stmt, 7, s->radius);
        sqlite3_bind_int (stmt, 8, (int)s->tool);
        sqlite3_bind_blob(stmt, 9, s->points, blob_size, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            fprintf(stderr, "db_save stroke %d step: %s\n", i, sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    return painting_id;
}

// ── Load ──────────────────────────────────────────────────────────────────────

bool db_load_painting(sqlite3 *db, int id,
                      Stroke **out_strokes, int *out_count,
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

    // Fetch strokes ordered by stroke_idx
    const char *sel_strokes =
        "SELECT color_r, color_g, color_b, color_a, radius, tool, points "
        "FROM strokes WHERE painting_id = ? ORDER BY stroke_idx ASC;";
    if (sqlite3_prepare_v2(db, sel_strokes, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);

    int capacity = 16, count = 0;
    Stroke *list = malloc(capacity * sizeof(Stroke));
    if (!list) { sqlite3_finalize(stmt); return false; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            Stroke *tmp = realloc(list, capacity * sizeof(Stroke));
            if (!tmp) break;
            list = tmp;
        }
        Stroke *s = &list[count++];
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

    *out_strokes = list;
    *out_count   = count;
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

void db_free_strokes(Stroke *strokes, int count) {
    for (int i = 0; i < count; i++) free(strokes[i].points);
    free(strokes);
}

// ── Delete ────────────────────────────────────────────────────────────────────

bool db_delete_painting(sqlite3 *db, int id) {
    // CASCADE handles strokes; need foreign_keys ON (set at open)
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "DELETE FROM paintings WHERE id = ?;",
                           -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}
