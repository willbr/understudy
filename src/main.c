#include <math.h>
#include <stdio.h>
#include <string.h>
#include "raylib.h"
#include "canvas.h"
#include "tools.h"
#include "toolbar.h"
#include "ui.h"
#include "db.h"
#include "font.h"

Font g_font;

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

    g_font = LoadFontEx("/System/Library/Fonts/SFNS.ttf", 32, NULL, 0);
    SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);

    canvas_init(&app.canvas);
    tools_init(&app.tools);
    ui_init(&app.ui);

    if (!db_open(&app.db))
        TraceLog(LOG_WARNING, "Could not open database — save/load disabled");

    float minimap_t = 0.0f;  // countdown; minimap visible while > 0
    bool  was_sizing = false;
    Vector2 size_anchor = {0}; // mouse position when D was pressed
    bool  was_zooming = false;
    Vector2 zoom_anchor = {0};
    bool  toolbar_hidden = false;
    bool  color_picker_open = false;
    Vector2 picker_pos = {0};
    bool  line_dragging = false;
    Vector2 line_start = {0};  // document-space
    bool  zoom_rect_active = false;
    Vector2 zoom_rect_start = {0};  // screen-space

    while (!WindowShouldClose()) {

        // ── Update ────────────────────────────────────────────────────────────
        if (IsKeyPressed(KEY_TAB) && app.ui.mode == UI_NONE) {
            toolbar_hidden = !toolbar_hidden;
            // Adjust view so canvas content doesn't shift
            if (toolbar_hidden)
                app.canvas.view_x += CANVAS_X;
            else
                app.canvas.view_x -= CANVAS_X;
            int cx = toolbar_hidden ? 0 : CANVAS_X;
            canvas_resize(&app.canvas, GetScreenWidth() - cx, GetScreenHeight());
        }

        int canvas_x = toolbar_hidden ? 0 : CANVAS_X;

        if (IsWindowResized())
            canvas_resize(&app.canvas,
                          GetScreenWidth() - canvas_x,
                          GetScreenHeight());

        if (app.ui.mode == UI_NONE) {
            ToolbarEvents ev = {0};
            if (!toolbar_hidden)
                ev = toolbar_update(&app.tools, &app.canvas);

            // ? = help overlay
            if (IsKeyPressed(KEY_SLASH) &&
                (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))) {
                app.ui.mode = UI_HELP;
            }

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
            if (ev.wants_export) {
                app.ui.mode           = UI_EXPORT_DIALOG;
                app.ui.text_len       = 0;
                app.ui.text_input[0]  = '\0';
                app.ui.cursor_blink_t = 0.0f;
            }
            if (ev.wants_crop) {
                app.ui.mode = UI_CROP_MODE;
                app.ui.crop_dragging   = false;
                app.ui.crop_rect_valid = false;
            }
            if (ev.wants_resize) {
                app.ui.mode = UI_RESIZE_DIALOG;
                snprintf(app.ui.resize_w_buf, 16, "%d", app.canvas.width);
                app.ui.resize_w_len = (int)strlen(app.ui.resize_w_buf);
                snprintf(app.ui.resize_h_buf, 16, "%d", app.canvas.height);
                app.ui.resize_h_len = (int)strlen(app.ui.resize_h_buf);
                app.ui.resize_lock_aspect = true;
                app.ui.resize_active_field = 0;
                app.ui.resize_aspect = (float)app.canvas.width / (float)app.canvas.height;
                app.ui.cursor_blink_t = 0.0f;
            }

            // Undo: Cmd+Z (macOS) or Ctrl+Z
            bool mod = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) ||
                       IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (mod && IsKeyPressed(KEY_Z))
                canvas_undo(&app.canvas);

            // Number keys = preset zoom levels (centered on current view)
            {
                float new_zoom = 0;
                if (IsKeyPressed(KEY_ONE))   new_zoom = 0.25f;
                if (IsKeyPressed(KEY_TWO))   new_zoom = 0.5f;
                if (IsKeyPressed(KEY_THREE)) new_zoom = 1.0f;
                if (IsKeyPressed(KEY_FOUR))  new_zoom = 2.0f;
                if (IsKeyPressed(KEY_FIVE))  new_zoom = 4.0f;
                if (new_zoom > 0 && new_zoom != app.canvas.zoom) {
                    int pw = app.canvas.rt.texture.width;
                    int ph = app.canvas.rt.texture.height;
                    // Anchor on center of viewport
                    float cx = (pw * 0.5f - app.canvas.view_x) / app.canvas.zoom;
                    float cy = (ph * 0.5f - app.canvas.view_y) / app.canvas.zoom;
                    app.canvas.view_x = pw * 0.5f - cx * new_zoom;
                    app.canvas.view_y = ph * 0.5f - cy * new_zoom;
                    app.canvas.zoom   = new_zoom;
                    canvas_redraw_for_view(&app.canvas);
                    minimap_t = 2.5f;
                }
            }

            // F held = show color picker; release picks hovered color
            if (IsKeyPressed(KEY_F)) {
                color_picker_open = true;
                picker_pos = GetMousePosition();
            }
            if (color_picker_open && IsKeyReleased(KEY_F)) {
                // Pick whatever swatch the cursor is hovering
                Vector2 m = GetMousePosition();
                float ring_r = 50.0f;
                static const Color PAL[12] = {
                    BLACK, DARKGRAY, GRAY, WHITE,
                    RED, ORANGE, YELLOW, GREEN,
                    SKYBLUE, BLUE, PURPLE, MAROON,
                };
                for (int i = 0; i < 12; i++) {
                    float angle = (float)i * (360.0f / 12.0f) * DEG2RAD;
                    float sx = picker_pos.x + cosf(angle) * ring_r;
                    float sy = picker_pos.y + sinf(angle) * ring_r;
                    float dx = m.x - sx, dy = m.y - sy;
                    if (dx * dx + dy * dy < 15.0f * 15.0f) {
                        app.tools.draw_color = PAL[i];
                        app.tools.active_tool = TOOL_BRUSH;
                        break;
                    }
                }
                color_picker_open = false;
            }

            // S held = line tool; release returns to brush
            if (IsKeyDown(KEY_S)) {
                app.tools.active_tool = TOOL_LINE;
            } else if (IsKeyReleased(KEY_S)) {
                app.tools.active_tool = TOOL_BRUSH;
                line_dragging = false;
            }

            // Z = zoom rect (hold Z, drag rect, release to zoom to fit)
            bool zoom_rect_key = IsKeyDown(KEY_Z) &&
                                 !IsKeyDown(KEY_LEFT_SUPER) && !IsKeyDown(KEY_RIGHT_SUPER) &&
                                 !IsKeyDown(KEY_LEFT_CONTROL) && !IsKeyDown(KEY_RIGHT_CONTROL);

            // Space = pan mode; D = brush size scrub; V = zoom scrub; E = eyedropper
            bool space = IsKeyDown(KEY_SPACE);
            bool sizing = IsKeyDown(KEY_D);
            bool zooming = IsKeyDown(KEY_V);
            bool eyedropper = IsKeyDown(KEY_E);
            if (was_sizing && !sizing) {
                ShowCursor();
                was_sizing = false;
            }
            if (was_zooming && !zooming) {
                ShowCursor();
                was_zooming = false;
            }

            // Scroll wheel → zoom anchored on cursor
            float wheel = GetMouseWheelMove();
            Vector2 mouse = GetMousePosition();
            if (wheel != 0.0f && mouse.x >= canvas_x) {
                // End any active stroke before changing the view transform
                if (app.canvas.is_drawing)
                    canvas_end_stroke(&app.canvas);

                float old_zoom = app.canvas.zoom;
                float new_zoom = old_zoom * powf(1.15f, wheel);
                if (new_zoom < 0.05f) new_zoom = 0.05f;
                if (new_zoom > 32.0f) new_zoom = 32.0f;

                // Keep the canvas point under the cursor fixed on screen
                float cx = (mouse.x - canvas_x - app.canvas.view_x) / old_zoom;
                float cy = (mouse.y - CANVAS_Y - app.canvas.view_y) / old_zoom;
                app.canvas.view_x = mouse.x - canvas_x - cx * new_zoom;
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
            } else if (zooming) {
                // Lock cursor; horizontal drag scrubs zoom anchored on cursor
                if (!was_zooming) {
                    zoom_anchor = GetMousePosition();
                    HideCursor();
                    was_zooming = true;
                }
                float dx = GetMouseDelta().x;
                SetMousePosition((int)zoom_anchor.x, (int)zoom_anchor.y);

                if (dx != 0.0f) {
                    float old_zoom = app.canvas.zoom;
                    float new_zoom = old_zoom * powf(1.01f, dx);
                    if (new_zoom < 0.05f) new_zoom = 0.05f;
                    if (new_zoom > 32.0f) new_zoom = 32.0f;

                    // Anchor zoom on the cursor position
                    float cx = (zoom_anchor.x - canvas_x - app.canvas.view_x) / old_zoom;
                    float cy = (zoom_anchor.y - CANVAS_Y - app.canvas.view_y) / old_zoom;
                    app.canvas.view_x = zoom_anchor.x - canvas_x - cx * new_zoom;
                    app.canvas.view_y = zoom_anchor.y - CANVAS_Y - cy * new_zoom;
                    app.canvas.zoom   = new_zoom;

                    canvas_redraw_for_view(&app.canvas);
                    minimap_t = 2.5f;
                }
                if (app.canvas.is_drawing)
                    canvas_end_stroke(&app.canvas);
            } else if (eyedropper) {
                SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && mouse.x >= canvas_x) {
                    // Sample color from the rendered RT at the mouse position
                    int px = (int)(mouse.x - canvas_x);
                    int py = (int)(mouse.y - CANVAS_Y);
                    Image img = LoadImageFromTexture(app.canvas.rt.texture);
                    ImageFlipVertical(&img);  // RT is Y-flipped
                    if (px >= 0 && px < img.width && py >= 0 && py < img.height) {
                        Color picked = GetImageColor(img, px, py);
                        if (picked.a > 0) {
                            app.tools.draw_color = picked;
                            app.tools.active_tool = TOOL_BRUSH;
                        }
                    }
                    UnloadImage(img);
                }
                if (app.canvas.is_drawing)
                    canvas_end_stroke(&app.canvas);
            } else if (zoom_rect_key) {
                SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && mouse.x >= canvas_x) {
                    zoom_rect_start = mouse;
                    zoom_rect_active = true;
                }
                if (zoom_rect_active && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    // Compute the selected rectangle in screen space
                    float x1 = fminf(zoom_rect_start.x, mouse.x);
                    float y1 = fminf(zoom_rect_start.y, mouse.y);
                    float x2 = fmaxf(zoom_rect_start.x, mouse.x);
                    float y2 = fmaxf(zoom_rect_start.y, mouse.y);
                    float rw = x2 - x1, rh = y2 - y1;

                    if (rw > 5 && rh > 5) {
                        // Center of selection in document space
                        float cx = (((x1 + x2) * 0.5f) - canvas_x - app.canvas.view_x) / app.canvas.zoom;
                        float cy = (((y1 + y2) * 0.5f) - CANVAS_Y - app.canvas.view_y) / app.canvas.zoom;

                        // Compute zoom to fit the rect
                        int panel_w = app.canvas.rt.texture.width;
                        int panel_h = app.canvas.rt.texture.height;
                        float doc_w = rw / app.canvas.zoom;
                        float doc_h = rh / app.canvas.zoom;
                        float new_zoom = fminf((float)panel_w / doc_w, (float)panel_h / doc_h);
                        if (new_zoom < 0.05f) new_zoom = 0.05f;
                        if (new_zoom > 32.0f) new_zoom = 32.0f;

                        // Center the view on the selection
                        app.canvas.view_x = (float)panel_w * 0.5f - cx * new_zoom;
                        app.canvas.view_y = (float)panel_h * 0.5f - cy * new_zoom;
                        app.canvas.zoom   = new_zoom;

                        canvas_redraw_for_view(&app.canvas);
                        minimap_t = 2.5f;
                    }
                    zoom_rect_active = false;
                }
                if (app.canvas.is_drawing)
                    canvas_end_stroke(&app.canvas);
            } else {
                zoom_rect_active = false;
                SetMouseCursor(MOUSE_CURSOR_DEFAULT);

                // Canvas stroke input (writes to RenderTexture)
                // Divide by zoom to convert screen → canvas-local coordinates
                Vector2 cpos  = {(mouse.x - canvas_x - app.canvas.view_x) / app.canvas.zoom,
                                 (mouse.y - CANVAS_Y - app.canvas.view_y) / app.canvas.zoom};
                bool on_canvas = (!color_picker_open &&
                                  mouse.x >= canvas_x &&
                                  cpos.x >= 0 && cpos.x < app.canvas.width &&
                                  cpos.y >= 0 && cpos.y < app.canvas.height);

                if (on_canvas) {
                    if (app.tools.active_tool == TOOL_LINE) {
                        // Line tool: click to set start, drag to preview, release to commit
                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                            line_start = cpos;
                            line_dragging = true;
                        }
                        if (line_dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                            // Live preview: render a temporary 2-point line stroke
                            Stroke preview = {0};
                            preview.color = tools_get_draw_color(&app.tools);
                            preview.radius = app.tools.brush_radius;
                            preview.tool = TOOL_LINE;
                            Vector2 pts[2] = {line_start, cpos};
                            preview.points = pts;
                            preview.count = 2;

                            BeginTextureMode(app.canvas.strokes_rt);
                            ClearBackground(BLANK);
                            for (int li = 0; li < app.canvas.layer_count; li++) {
                                if (!app.canvas.layers[li].visible) continue;
                                Layer *l = &app.canvas.layers[li];
                                for (int si = 0; si < l->stroke_count; si++)
                                    render_stroke_transformed(&l->strokes[si],
                                        app.canvas.view_x, app.canvas.view_y, app.canvas.zoom);
                            }
                            render_stroke_transformed(&preview,
                                app.canvas.view_x, app.canvas.view_y, app.canvas.zoom);
                            EndTextureMode();
                            composite_ink(&app.canvas);

                            preview.points = NULL; // don't free stack array
                        }
                        if (line_dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                            // Commit a 2-point stroke
                            Color color = tools_get_draw_color(&app.tools);
                            canvas_begin_stroke(&app.canvas, color,
                                                app.tools.brush_radius,
                                                TOOL_LINE);
                            canvas_add_point(&app.canvas, line_start);
                            canvas_add_point(&app.canvas, cpos);
                            canvas_end_stroke(&app.canvas);
                            line_dragging = false;
                        }
                    } else {
                        // Brush/Eraser: freehand drawing
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
                }

                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && app.tools.active_tool != TOOL_LINE)
                    canvas_end_stroke(&app.canvas);
            }
        }

        // ── Draw ──────────────────────────────────────────────────────────────
        BeginDrawing();
            ClearBackground((Color){25, 25, 25, 255});

            canvas_draw(&app.canvas, canvas_x);

            // Zoom rect preview
            if (zoom_rect_active && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                Vector2 m = GetMousePosition();
                float x1 = fminf(zoom_rect_start.x, m.x);
                float y1 = fminf(zoom_rect_start.y, m.y);
                float rw = fabsf(m.x - zoom_rect_start.x);
                float rh = fabsf(m.y - zoom_rect_start.y);
                DrawRectangleLinesEx((Rectangle){x1, y1, rw, rh}, 1.5f,
                                     (Color){255, 200, 50, 220});
                DrawRectangle((int)x1, (int)y1, (int)rw, (int)rh,
                              (Color){255, 200, 50, 30});
            }

            if (!toolbar_hidden)
                toolbar_draw(&app.tools, &app.canvas);

            // Collapse/expand toggle button
            {
                const char *icon = toolbar_hidden ? ">" : "<";
                int bx = toolbar_hidden ? 4 : CANVAS_X - 18;
                Rectangle tog = {(float)bx, 4, 16, 24};
                bool hovered = CheckCollisionPointRec(GetMousePosition(), tog);
                DrawRectangleRec(tog, hovered ? (Color){70, 70, 70, 255}
                                              : (Color){50, 50, 50, 255});
                DrawUI(icon, bx + 4, 8, 14, (Color){180, 180, 180, 255});
                if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    toolbar_hidden = !toolbar_hidden;
                    if (toolbar_hidden)
                        app.canvas.view_x += CANVAS_X;
                    else
                        app.canvas.view_x -= CANVAS_X;
                    canvas_x = toolbar_hidden ? 0 : CANVAS_X;
                    canvas_resize(&app.canvas, GetScreenWidth() - canvas_x,
                                  GetScreenHeight());
                }
            }

            // Status line: layer + zoom
            char sc[96];
            snprintf(sc, sizeof(sc), "Layer %d/%d  %d strokes  %d%%",
                     app.canvas.active_layer + 1,
                     app.canvas.layer_count,
                     app.canvas.layers[app.canvas.active_layer].stroke_count,
                     (int)(app.canvas.zoom * 100.0f + 0.5f));
            DrawUI(sc, 10, GetScreenHeight() - 20, 12, (Color){100, 100, 100, 255});

            if (app.canvas.dirty)
                DrawUI("*", GetScreenWidth() - 20, 5, 16, YELLOW);

            // Minimap — fade in on view change, fade out after 2.5 s
            minimap_t -= GetFrameTime();
            if (minimap_t < 0.0f) minimap_t = 0.0f;
            float mm_alpha = minimap_t > 1.0f ? 1.0f : minimap_t;
            canvas_draw_minimap(&app.canvas, mm_alpha, canvas_x);

            // Color picker ring
            if (color_picker_open) {
                static const Color PAL[12] = {
                    BLACK, DARKGRAY, GRAY, WHITE,
                    RED, ORANGE, YELLOW, GREEN,
                    SKYBLUE, BLUE, PURPLE, MAROON,
                };
                float ring_r = 50.0f;
                float swatch_r = 14.0f;
                // Background disc
                DrawCircleV(picker_pos, ring_r + swatch_r + 6, (Color){30, 30, 30, 200});
                DrawCircleLinesV(picker_pos, ring_r + swatch_r + 6, (Color){80, 80, 80, 200});
                // Swatches
                Vector2 mouse_now = GetMousePosition();
                for (int i = 0; i < 12; i++) {
                    float angle = (float)i * (360.0f / 12.0f) * DEG2RAD;
                    float sx = picker_pos.x + cosf(angle) * ring_r;
                    float sy = picker_pos.y + sinf(angle) * ring_r;
                    float dx = mouse_now.x - sx, dy = mouse_now.y - sy;
                    bool hov = (dx * dx + dy * dy < swatch_r * swatch_r);
                    DrawCircleV((Vector2){sx, sy}, hov ? swatch_r + 2 : swatch_r, PAL[i]);
                    DrawCircleLinesV((Vector2){sx, sy}, hov ? swatch_r + 2 : swatch_r,
                                     hov ? WHITE : (Color){100, 100, 100, 255});
                }
                // Current color in center
                DrawCircleV(picker_pos, 12, app.tools.draw_color);
                DrawCircleLinesV(picker_pos, 12, (Color){150, 150, 150, 255});
            }

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
                    int tw = MeasureUI(sz, 16);
                    int tx = (int)size_anchor.x + (int)r + 8;
                    int ty = (int)size_anchor.y - 8;
                    DrawRectangle(tx - 3, ty - 2, tw + 6, 20,
                                  (Color){30, 30, 30, 200});
                    DrawUI(sz, tx, ty, 16, WHITE);
                } else if (app.ui.mode == UI_NONE && !panning &&
                           !IsKeyDown(KEY_Z) && !IsKeyDown(KEY_E) && m.x >= canvas_x) {
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
                ui_update(&app.ui, &app.canvas, app.db, canvas_x);
                ui_draw(&app.ui, &app.canvas, canvas_x);
            }

        EndDrawing();
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    ui_free(&app.ui);
    canvas_free(&app.canvas);
    UnloadFont(g_font);
    if (app.db) db_close(app.db);
    CloseWindow();
    return 0;
}
