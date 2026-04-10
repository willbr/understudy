#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *CREATE_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS paintings ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name       TEXT    NOT NULL,"
    "  created_at TEXT    NOT NULL DEFAULT (datetime('now')),"
    "  updated_at TEXT    NOT NULL DEFAULT (datetime('now')),"
    "  width      INTEGER NOT NULL,"
    "  height     INTEGER NOT NULL,"
    "  pixel_data BLOB    NOT NULL"
    ");";

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

    sqlite3_exec(*db, "PRAGMA journal_mode=WAL;",    NULL, NULL, NULL);
    sqlite3_exec(*db, "PRAGMA synchronous=NORMAL;",  NULL, NULL, NULL);

    char *err = NULL;
    if (sqlite3_exec(*db, CREATE_TABLE_SQL, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "db_open create table: %s\n", err);
        sqlite3_free(err);
        return false;
    }

    return true;
}

void db_close(sqlite3 *db) {
    sqlite3_close(db);
}

bool db_save_painting(sqlite3 *db, const char *name,
                      const unsigned char *blob, int blob_size,
                      int width, int height) {
    const char *sql =
        "INSERT INTO paintings (name, width, height, pixel_data) VALUES (?, ?, ?, ?);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_save prepare: %s\n", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, width);
    sqlite3_bind_int (stmt, 3, height);
    sqlite3_bind_blob(stmt, 4, blob, blob_size, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok) fprintf(stderr, "db_save step: %s\n", sqlite3_errmsg(db));

    sqlite3_finalize(stmt);
    return ok;
}

bool db_load_painting(sqlite3 *db, int id,
                      unsigned char **out_blob, int *out_blob_size,
                      int *out_width, int *out_height) {
    const char *sql =
        "SELECT width, height, pixel_data FROM paintings WHERE id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_load prepare: %s\n", sqlite3_errmsg(db));
        return false;
    }

    sqlite3_bind_int(stmt, 1, id);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    *out_width     = sqlite3_column_int(stmt, 0);
    *out_height    = sqlite3_column_int(stmt, 1);
    *out_blob_size = sqlite3_column_bytes(stmt, 2);

    const void *raw = sqlite3_column_blob(stmt, 2);
    *out_blob = (unsigned char *)malloc(*out_blob_size);
    if (*out_blob) memcpy(*out_blob, raw, *out_blob_size);

    sqlite3_finalize(stmt);
    return (*out_blob != NULL);
}

bool db_list_paintings(sqlite3 *db, PaintingMeta **out_list, int *out_count) {
    const char *sql =
        "SELECT id, name, created_at, width, height "
        "FROM paintings ORDER BY updated_at DESC;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "db_list prepare: %s\n", sqlite3_errmsg(db));
        return false;
    }

    int capacity = 16;
    int count    = 0;
    PaintingMeta *list = (PaintingMeta *)malloc(capacity * sizeof(PaintingMeta));
    if (!list) { sqlite3_finalize(stmt); return false; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= capacity) {
            capacity *= 2;
            PaintingMeta *tmp = (PaintingMeta *)realloc(list, capacity * sizeof(PaintingMeta));
            if (!tmp) break;
            list = tmp;
        }
        PaintingMeta *m = &list[count++];
        m->id = sqlite3_column_int(stmt, 0);
        strncpy(m->name,       (const char *)sqlite3_column_text(stmt, 1), sizeof(m->name) - 1);
        strncpy(m->created_at, (const char *)sqlite3_column_text(stmt, 2), sizeof(m->created_at) - 1);
        m->width  = sqlite3_column_int(stmt, 3);
        m->height = sqlite3_column_int(stmt, 4);
        m->name[sizeof(m->name) - 1]             = '\0';
        m->created_at[sizeof(m->created_at) - 1] = '\0';
    }

    sqlite3_finalize(stmt);
    *out_list  = list;
    *out_count = count;
    return true;
}

void db_free_list(PaintingMeta *list) {
    free(list);
}

bool db_delete_painting(sqlite3 *db, int id) {
    const char *sql = "DELETE FROM paintings WHERE id = ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}
