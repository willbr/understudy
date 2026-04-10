#include "raylib.h"
#include "canvas.h"
#include "tools.h"
#include "toolbar.h"
#include "ui.h"
#include "db.h"

#define WIN_W  1280
#define WIN_H  800
#define TARGET_FPS 60

typedef struct {
    Canvas   canvas;
    ToolState tools;
    UIState  ui;
    sqlite3  *db;
} AppState;

int main(void) {
    AppState app = {0};

    InitWindow(WIN_W, WIN_H, "Claude Paint");
    SetTargetFPS(TARGET_FPS);
    SetExitKey(KEY_NULL); // Don't quit on Escape (we use it in modals)

    canvas_init(&app.canvas);
    tools_init(&app.tools);
    ui_init(&app.ui);

    if (!db_open(&app.db)) {
        // Non-fatal: saving/loading will be unavailable
        TraceLog(LOG_WARNING, "Could not open database — save/load disabled");
    }

    while (!WindowShouldClose()) {

        // ── Input ─────────────────────────────────────────────────────────────
        if (app.ui.mode != UI_NONE) {
            // Modal consumes all input
            ui_update(&app.ui, &app.canvas, app.db);
        } else {
            ToolbarEvents ev = toolbar_update(&app.tools);

            if (ev.wants_new) {
                if (app.canvas.dirty) {
                    app.ui.mode = UI_CONFIRM_NEW;
                } else {
                    canvas_clear(&app.canvas);
                }
            }
            if (ev.wants_save) {
                if (app.db) {
                    app.ui.mode     = UI_SAVE_DIALOG;
                    app.ui.text_len = 0;
                    app.ui.text_input[0] = '\0';
                    app.ui.cursor_blink_t = 0.0f;
                }
            }
            if (ev.wants_load) {
                if (app.db) {
                    ui_free(&app.ui);
                    db_list_paintings(app.db, &app.ui.load_list, &app.ui.load_count);
                    app.ui.load_scroll   = 0;
                    app.ui.load_selected = app.ui.load_count > 0 ? 0 : -1;
                    app.ui.mode = UI_LOAD_LIST;
                }
            }

            // Canvas drawing (only when mouse is in canvas area)
            Vector2 mouse = GetMousePosition();
            bool in_canvas = (mouse.x >= CANVAS_X && mouse.x < CANVAS_X + CANVAS_WIDTH &&
                              mouse.y >= CANVAS_Y && mouse.y < CANVAS_Y + CANVAS_HEIGHT);

            if (in_canvas) {
                // Convert to canvas-local coordinates
                Vector2 canvas_pos = {mouse.x - CANVAS_X, mouse.y - CANVAS_Y};

                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    app.canvas.is_drawing = true;
                    app.canvas.last_mouse = canvas_pos;
                }

                if (app.canvas.is_drawing && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    Color color = tools_get_draw_color(&app.tools);
                    canvas_paint(&app.canvas,
                                 app.canvas.last_mouse, canvas_pos,
                                 color, app.tools.brush_radius);
                    app.canvas.last_mouse = canvas_pos;
                }
            }

            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                app.canvas.is_drawing = false;
            }
        }

        // ── Draw ──────────────────────────────────────────────────────────────
        BeginDrawing();
            ClearBackground((Color){25, 25, 25, 255});

            canvas_draw(&app.canvas);
            toolbar_draw(&app.tools);

            if (app.ui.mode != UI_NONE)
                ui_draw(&app.ui);

            // Dirty indicator in title bar
            if (app.canvas.dirty)
                DrawText("*", WIN_W - 20, 5, 16, YELLOW);

        EndDrawing();
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    ui_free(&app.ui);
    canvas_free(&app.canvas);
    if (app.db) db_close(app.db);
    CloseWindow();

    return 0;
}
