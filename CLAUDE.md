# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
make          # build → ./claude-paint
make run      # build + launch
make clean    # remove objects and binary
```

Single-file rebuild (faster iteration):
```bash
clang -std=c11 -Wall -Wextra -g $(pkg-config --cflags raylib sqlite3) -c src/canvas.c -o src/canvas.o
```

## Architecture

1280×800 window split into a 220px toolbar (left) and 1060×800 canvas (right). No HiDPI — mouse coords are always logical pixels.

### Modules

| File | Owns |
|------|------|
| `main.c` | `AppState`, game loop, input routing |
| `canvas.c/.h` | `Image` (CPU) + `Texture2D` (GPU), stroke drawing, PNG blob I/O |
| `tools.c/.h` | `ToolState` — active tool, color, brush radius (pure data, no rendering) |
| `toolbar.c/.h` | Left panel draw + hit-test; writes into `ToolState`, sets `UIState.mode` |
| `db.c/.h` | All SQLite: open/close, save/load/list paintings as PNG BLOBs |
| `ui.c/.h` | Save-name dialog and load-list modal overlays |

### Data flow

```
main loop
  if ui.mode != UI_NONE → ui_update() consumes all input
  else:
    toolbar_update()  writes tools.*  and sets ui.mode
    canvas_update()   reads  tools.*  draws on canvas
  ---
  canvas_draw()
  toolbar_draw()
  ui_draw()          (on top if active)
```

### Canvas — vector strokes

The canvas is **vector**: strokes are recorded as arrays of `Vector2` sample points and replayed at render time. The `RenderTexture2D` is an accumulated render cache, updated incrementally as the user draws.

Stroke lifecycle in `main.c`:
```
IsMouseButtonPressed → canvas_begin_stroke()
IsMouseButtonDown    → canvas_add_point()   ← renders segment to RT via BeginTextureMode
IsMouseButtonReleased→ canvas_end_stroke()  ← commits stroke to strokes[]
Cmd/Ctrl+Z           → canvas_undo()        ← pops last stroke, replays all remaining
```

`canvas_add_point` renders one stroke segment incrementally. `canvas_undo` replays all strokes from scratch via `redraw_all()`. `BeginTextureMode` / `EndTextureMode` is called from the update phase, before `BeginDrawing`.

### Canvas storage (SQLite)

Two tables. Each stroke is one row; points packed as a raw `Vector2` (float[2]) BLOB.  
DB: `~/Library/Application Support/claude-paint/paintings.db`

```sql
paintings(id, name, created_at, updated_at, width, height)
strokes(id, painting_id→paintings.id CASCADE, stroke_idx, color_r/g/b/a, radius, tool, points BLOB)
```

Key rules:
- `PRAGMA foreign_keys = ON` enables cascade delete (removing a painting removes its strokes).
- Always `sqlite3_bind_blob(..., SQLITE_TRANSIENT)` — SQLite copies before the buffer is freed.
- `db_free_strokes(strokes, count)` frees the loaded stroke array returned by `db_load_painting`.
- `canvas_load_strokes` takes ownership of the `Stroke *` array from `db_load_painting`.

## Dependencies

- raylib 5.5 (`brew install raylib`)
- sqlite3 (`brew install sqlite`)
- macOS frameworks in Makefile: `IOKit Cocoa OpenGL CoreVideo`
