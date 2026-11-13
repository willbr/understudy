# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repo.

## Workflow

Always commit your changes when a piece of work is done — don't leave them uncommitted in the working tree or wait to be asked. Commit directly to `master`; no feature branches.

## Build & Run

```bash
zig build                        # → zig-out/bin/understudy(.exe), ReleaseFast by default
zig build run                    # build + launch
zig build -Doptimize=Debug       # debug build (stroke rasterization is several times slower)
```

Requires Zig 0.15.2+ (0.16 also works). First build fetches raylib source into `deps/` via `curl` + `tar` (present on macOS and Windows 10+); sqlite is pulled through the Zig package manager.

## Dependencies

- raylib 5.5 — vendored into `deps/raylib-5.5/` by `build.zig` on first build (upstream's own `build.zig` targets Zig 0.13/0.14 and breaks on 0.15, so we compile the C sources directly).
- sqlite3 amalgamation — declared in `build.zig.zon`, fetched by the Zig package manager.
- macOS frameworks: `IOKit Cocoa OpenGL CoreAudio CoreVideo`
- Windows system libs: `opengl32 gdi32 winmm shell32 user32`

Platform clipboard: `clipboard_mac.m` (AppKit NSPasteboard) on macOS, `clipboard_win.c` (Win32 + stb_image_write for CF_DIB→PNG) on Windows. `db.c` picks `%APPDATA%\Understudy` on Windows and `~/Library/Application Support/Understudy` on macOS.

## Window & coordinates

Resizable window, starts 1280×800 and maximizes. Left toolbar is 220 px (`CANVAS_X`), canvas panel fills the remainder. `Tab` hides the toolbar. No HiDPI scaling — mouse coords are logical pixels.

The document is fixed at 4096×4096 (`CANVAS_DOC_W/H`); users pan and zoom within it. Screen ↔ document conversion uses `view_x/y` and `zoom` on `Canvas`.

## Modules

| File | Owns |
|------|------|
| `main.c` | `AppState`, game loop, all keyboard routing |
| `canvas.c/.h` | `Canvas`, `Layer`, `Stroke`; render targets; shader compositing |
| `refimage.h` | Single-header ref-image system (images dropped/pasted onto the canvas) |
| `tools.c/.h` | `ToolState` — active tool, color, brush radius (pure data) |
| `toolbar.c/.h` | Left-panel draw + hit-test; emits `ToolbarEvents` |
| `db.c/.h` | SQLite: paintings, layers, strokes, ref images, autosave |
| `ui.c/.h` | Modal overlays (save, load, export, crop, resize, help, layer settings) |
| `clipboard_mac.m` | `clipboard_image_png()` — NSPasteboard → PNG bytes |
| `font.h` | `DrawUI` / `MeasureUI` wrappers around the global `g_font` |
| `paper.fs`, `ink.fs` | GLSL fragment shaders for paper texture and ink compositing |

## Data flow

```
main loop
  if ui.mode != UI_NONE → ui_update() consumes all input
  else:
    refimage_update()  — click/drag/rename ref images
    toolbar_update()   — emits events; writes tools.*
    key routing        — modes (space/D/G/Z/E/F/Shift) and actions (N/H/L, 1–5, Cmd-Z, Cmd-V)
    canvas stroke I/O  — begin/add/end for brush, eraser, line, pan-layer
  draw:
    dark bg → paper → z-sorted layers+refs → overlays (selection, minimap, picker, brush cursor)
    toolbar → modals on top
```

## Canvas — vector strokes, layered

Strokes are arrays of `Vector2` sample points plus color, radius, tool. Each `Layer` owns its own stroke array and a per-layer `pan_x/pan_y`, `opacity`, `visible`, and `z` (unified z-order shared with ref images).

Render targets:
- `Layer.cache_rt` — per-layer raster of that layer's committed strokes, tagged with the view it was rasterized at. `canvas_draw_layer()` re-rasterizes it synchronously on content change (`cache_dirty`). After a view change, an expensive layer shows its stale cache translated/scaled while a background job re-rasterizes it into `job_rt` a couple of ms per frame, then swaps it in — never a whole rasterization in one frame.
- `Layer.overview_rt` — 1024² mipmapped raster of the whole document for that layer. Drawn under the stale cache after a view change so areas the cache doesn't cover aren't blank. New strokes are stamped onto it in `canvas_end_stroke()`; any other content change invalidates it and the background job rebuilds it.
- `live_rt` — active layer's cache + the settled part of the in-progress stroke, so each frame renders only newly added segments. Swapped in as the layer's cache on stroke end.
- `strokes_rt` — scratch: one layer's strokes for the current frame, fed to `ink.fs`
- `paper_rt` — paper texture at the current view (from `paper.fs`)
- `minimap_rt` — thumbnail of the whole document
- `rt` — final panel-sized output

Stroke lifecycle:
```
IsMouseButtonPressed  → canvas_begin_stroke()
IsMouseButtonDown     → canvas_add_point()    — records a sample (drawn by canvas_draw_layer)
IsMouseButtonReleased → canvas_end_stroke()   — commits to active layer, folds into its cache
Cmd/Ctrl+Z            → canvas_undo()         — pops last stroke, replays
```

After any view change (pan/zoom/resize), call `canvas_redraw_for_view()` to re-render the paper; layer caches detect the new view themselves.

## Ref images

`refimage.h` is `#define REFIMAGE_IMPLEMENTATION`'d once in `main.c`. It owns its own array of `RefImage`s with position, rotation, scale, z, lock, name. Dropped files and clipboard-pasted PNGs both go through `refimage_add()`. Ref images and stroke layers share the same `z` axis and are merged and z-sorted at draw time in `main.c`.

## Storage (SQLite)

DB: `~/Library/Application Support/Understudy/paintings.db`

Tables:
- `paintings(id, name, created_at, updated_at, width, height, is_autosave)`
- `layers(id, painting_id→paintings CASCADE, layer_idx, name, visible, z, pan_x, pan_y, opacity)`
- `strokes(id, layer_id→layers CASCADE, stroke_idx, color_r/g/b/a, radius, tool, points BLOB)`
- `ref_images(id, painting_id→paintings CASCADE, z, x, y, scale, rotation, locked, name, png BLOB)`

Rules:
- `PRAGMA foreign_keys = ON` for cascade delete.
- Always `sqlite3_bind_blob(..., SQLITE_TRANSIENT)` — SQLite copies before the buffer is freed.
- Points packed as raw `Vector2` (float[2]) BLOBs.
- Autosave: `db_autosave()` overwrites the same row every stroke-end; its id is tracked in `main.c` as `autosave_id` (0 until first save).
- `db_free_layers` / `db_free_ref_images` free the arrays returned by the loaders.

## Keyboard (reference)

Held-key modes and one-shots live in `main.c`. The `/` overlay (`UI_HELP`) is the source of truth for users — if you add or change a binding, update `ui.c`'s help panel too.
