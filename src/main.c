#include <math.h>
#include <stdio.h>
#include "raylib.h"
#include "canvas.h"
#include "tools.h"
#include "toolbar.h"
#include "ui.h"
#include "db.h"

#define TARGET_FPS 60

typedef struct {
    Canvas    canvas;
    ToolState tools;
    UIState   ui;
    sqlite3  *db;
} AppState;

int main(void) {
    AppState app = {0};

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 800, "Claude Paint");
    SetTargetFPS(TARGET_FPS);
    SetExitKey(KEY_NULL); // Escape is used by modals

    canvas_init(&app.canvas);
    tools_init(&app.tools);
    ui_init(&app.ui);

    if (!db_open(&app.db))
        TraceLog(LOG_WARNING, "Could not open database — save/load disabled");

    float minimap_t = 0.0f;  // countdown; minimap visible while > 0

    while (!WindowShouldClose()) {

        // ── Update ────────────────────────────────────────────────────────────
        if (IsWindowResized())
            canvas_resize(&app.canvas,
                          GetScreenWidth() - CANVAS_X,
                          GetScreenHeight());

        if (app.ui.mode == UI_NONE) {
            ToolbarEvents ev = toolbar_update(&app.tools);

            // Toolbar button actions
            if (ev.wants_new) {
                if (app.canvas.dirty) {
                    app.ui.mode = UI_CONFIRM_NEW;
                } else {
                    canvas_clear(&app.canvas);
                }
            }
            if (ev.wants_save && app.db) {
                app.ui.mode           = UI_SAVE_DIALOG;
                app.ui.text_len       = 0;
                app.ui.text_input[0]  = '\0';
                app.ui.cursor_blink_t = 0.0f;
            }
            if (ev.wants_load && app.db) {
                ui_free(&app.ui);
                db_list_paintings(app.db, &app.ui.load_list, &app.ui.load_count);
                app.ui.load_scroll   = 0;
                app.ui.load_selected = app.ui.load_count > 0 ? 0 : -1;
                app.ui.mode          = UI_LOAD_LIST;
            }

            // Undo: Cmd+Z (macOS) or Ctrl+Z
            bool mod = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) ||
                       IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (mod && IsKeyPressed(KEY_Z))
                canvas_undo(&app.canvas);

            // Space = pan mode; otherwise draw
            bool space = IsKeyDown(KEY_SPACE);

            // Scroll wheel → zoom anchored on cursor
            float wheel = GetMouseWheelMove();
            Vector2 mouse = GetMousePosition();
            if (wheel != 0.0f && mouse.x >= CANVAS_X) {
                // End any active stroke before changing the view transform
                if (app.canvas.is_drawing)
                    canvas_end_stroke(&app.canvas);

                float old_zoom = app.canvas.zoom;
                float new_zoom = old_zoom * powf(1.15f, wheel);
                if (new_zoom < 0.05f) new_zoom = 0.05f;
                if (new_zoom > 32.0f) new_zoom = 32.0f;

                // Keep the canvas point under the cursor fixed on screen
                float cx = (mouse.x - CANVAS_X - app.canvas.view_x) / old_zoom;
                float cy = (mouse.y - CANVAS_Y - app.canvas.view_y) / old_zoom;
                app.canvas.view_x = mouse.x - CANVAS_X - cx * new_zoom;
                app.canvas.view_y = mouse.y - CANVAS_Y - cy * new_zoom;
                app.canvas.zoom   = new_zoom;

                canvas_redraw_for_view(&app.canvas);
                minimap_t = 2.5f;
            }

            if (space) {
                SetMouseCursor(IsMouseButtonDown(MOUSE_BUTTON_LEFT)
                               ? MOUSE_CURSOR_RESIZE_ALL
                               : MOUSE_CURSOR_POINTING_HAND);
                minimap_t = 0.3f;  // keep refreshing while space held
                if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    Vector2 delta = GetMouseDelta();
                    app.canvas.view_x += delta.x;
                    app.canvas.view_y += delta.y;
                    canvas_redraw_for_view(&app.canvas);
                    minimap_t = 0.3f;
                }
                // Cancel any stroke that was in progress when Space was pressed
                if (app.canvas.is_drawing)
                    canvas_end_stroke(&app.canvas);
            } else {
                SetMouseCursor(MOUSE_CURSOR_DEFAULT);

                // Canvas stroke input (writes to RenderTexture)
                // Divide by zoom to convert screen → canvas-local coordinates
                Vector2 cpos  = {(mouse.x - CANVAS_X - app.canvas.view_x) / app.canvas.zoom,
                                 (mouse.y - CANVAS_Y - app.canvas.view_y) / app.canvas.zoom};
                bool on_canvas = (mouse.x >= CANVAS_X &&
                                  cpos.x >= 0 && cpos.x < app.canvas.width &&
                                  cpos.y >= 0 && cpos.y < app.canvas.height);

                if (on_canvas) {
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        Color color = tools_get_draw_color(&app.tools);
                        canvas_begin_stroke(&app.canvas, color,
                                            app.tools.brush_radius,
                                            app.tools.active_tool);
                        canvas_add_point(&app.canvas, cpos);
                    } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && app.canvas.is_drawing) {
                        canvas_add_point(&app.canvas, cpos);
                    }
                }

                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                    canvas_end_stroke(&app.canvas);
            }
        }

        // ── Draw ──────────────────────────────────────────────────────────────
        BeginDrawing();
            ClearBackground((Color){25, 25, 25, 255});

            canvas_draw(&app.canvas);
            toolbar_draw(&app.tools);

            // Status line: stroke count + zoom level
            char sc[64];
            snprintf(sc, sizeof(sc), "%d strokes  %d%%",
                     app.canvas.stroke_count,
                     (int)(app.canvas.zoom * 100.0f + 0.5f));
            DrawText(sc, 10, GetScreenHeight() - 20, 12, (Color){100, 100, 100, 255});

            if (app.canvas.dirty)
                DrawText("*", GetScreenWidth() - 20, 5, 16, YELLOW);

            // Minimap — fade in on view change, fade out after 2.5 s
            minimap_t -= GetFrameTime();
            if (minimap_t < 0.0f) minimap_t = 0.0f;
            float mm_alpha = minimap_t > 1.0f ? 1.0f : minimap_t;
            canvas_draw_minimap(&app.canvas, mm_alpha);

            // Modals: immediate-mode (input + draw combined, inside BeginDrawing)
            if (app.ui.mode != UI_NONE) {
                ui_update(&app.ui, &app.canvas, app.db);
                ui_draw(&app.ui);
            }

        EndDrawing();
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    ui_free(&app.ui);
    canvas_free(&app.canvas);
    if (app.db) db_close(app.db);
    CloseWindow();
    return 0;
}
