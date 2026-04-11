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
    bool  was_sizing = false;
    Vector2 size_anchor = {0}; // mouse position when D was pressed

    while (!WindowShouldClose()) {

        // ── Update ────────────────────────────────────────────────────────────
        if (IsWindowResized())
            canvas_resize(&app.canvas,
                          GetScreenWidth() - CANVAS_X,
                          GetScreenHeight());

        if (app.ui.mode == UI_NONE) {
            ToolbarEvents ev = toolbar_update(&app.tools, &app.canvas);

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

            // Space = pan mode; D = brush size scrub; otherwise draw
            bool space = IsKeyDown(KEY_SPACE);
            bool sizing = IsKeyDown(KEY_D);
            if (was_sizing && !sizing) {
                ShowCursor();
                was_sizing = false;
            }

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
            } else if (sizing) {
                // Lock cursor in place; horizontal movement scrubs brush size
                if (!was_sizing) {
                    size_anchor = GetMousePosition();
                    HideCursor();
                    was_sizing = true;
                }
                float dx = GetMouseDelta().x;
                SetMousePosition((int)size_anchor.x, (int)size_anchor.y);
                app.tools.brush_radius += (int)(dx * 0.15f);
                if (app.tools.brush_radius < 1)  app.tools.brush_radius = 1;
                if (app.tools.brush_radius > 200) app.tools.brush_radius = 200;
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
            toolbar_draw(&app.tools, &app.canvas);

            // Status line: layer + zoom
            char sc[96];
            snprintf(sc, sizeof(sc), "Layer %d/%d  %d strokes  %d%%",
                     app.canvas.active_layer + 1,
                     app.canvas.layer_count,
                     app.canvas.layers[app.canvas.active_layer].stroke_count,
                     (int)(app.canvas.zoom * 100.0f + 0.5f));
            DrawText(sc, 10, GetScreenHeight() - 20, 12, (Color){100, 100, 100, 255});

            if (app.canvas.dirty)
                DrawText("*", GetScreenWidth() - 20, 5, 16, YELLOW);

            // Minimap — fade in on view change, fade out after 2.5 s
            minimap_t -= GetFrameTime();
            if (minimap_t < 0.0f) minimap_t = 0.0f;
            float mm_alpha = minimap_t > 1.0f ? 1.0f : minimap_t;
            canvas_draw_minimap(&app.canvas, mm_alpha);

            // Brush cursor — circle showing brush size
            {
                Vector2 m = GetMousePosition();
                bool panning = IsKeyDown(KEY_SPACE);
                if (was_sizing) {
                    // Show brush circle + size label at the anchor point
                    float r = fmaxf(1.5f, (float)app.tools.brush_radius * app.canvas.zoom);
                    DrawRing((Vector2){size_anchor.x, size_anchor.y}, r + 1, r + 2.5f, 0, 360, 36,
                             (Color){0, 0, 0, 180});
                    DrawRing((Vector2){size_anchor.x, size_anchor.y}, r, r + 1, 0, 360, 36,
                             (Color){255, 255, 255, 220});
                    DrawLine((int)size_anchor.x - 4, (int)size_anchor.y, (int)size_anchor.x + 4, (int)size_anchor.y,
                             (Color){255, 255, 255, 180});
                    DrawLine((int)size_anchor.x, (int)size_anchor.y - 4, (int)size_anchor.x, (int)size_anchor.y + 4,
                             (Color){255, 255, 255, 180});
                    char sz[16];
                    snprintf(sz, sizeof(sz), "%d", app.tools.brush_radius);
                    int tw = MeasureText(sz, 16);
                    int tx = (int)size_anchor.x + (int)r + 8;
                    int ty = (int)size_anchor.y - 8;
                    DrawRectangle(tx - 3, ty - 2, tw + 6, 20,
                                  (Color){30, 30, 30, 200});
                    DrawText(sz, tx, ty, 16, WHITE);
                } else if (app.ui.mode == UI_NONE && !panning && m.x >= CANVAS_X) {
                    HideCursor();
                    float r = fmaxf(1.5f, (float)app.tools.brush_radius * app.canvas.zoom);
                    // Dark outline + white inline for contrast on any background
                    DrawRing((Vector2){m.x, m.y}, r + 1, r + 2.5f, 0, 360, 36,
                             (Color){0, 0, 0, 180});
                    DrawRing((Vector2){m.x, m.y}, r, r + 1, 0, 360, 36,
                             (Color){255, 255, 255, 220});
                    // Crosshair at center
                    DrawLine((int)m.x - 4, (int)m.y, (int)m.x + 4, (int)m.y,
                             (Color){255, 255, 255, 180});
                    DrawLine((int)m.x, (int)m.y - 4, (int)m.x, (int)m.y + 4,
                             (Color){255, 255, 255, 180});
                } else {
                    ShowCursor();
                }
            }

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
