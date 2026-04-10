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

### Canvas storage (SQLite)

Paintings are stored as raw PNG bytes (`ExportImageToMemory` → `BLOB`).  
DB: `~/Library/Application Support/claude-paint/paintings.db`

```sql
paintings(id, name, created_at, updated_at, width, height, pixel_data BLOB)
```

Key rules:
- Always `sqlite3_bind_blob(..., SQLITE_TRANSIENT)` for BLOBs — never string interpolation.
- Use `MemFree()` (not `free()`) for raylib-allocated PNG buffers from `ExportImageToMemory`.
- `UpdateTexture` is called once per `canvas_paint()` call, not once per interpolated stamp.

### Stroke interpolation

`canvas_paint(from, to, color, radius)` lerps between the two positions and stamps `ImageDrawCircle` every `radius/2` pixels to avoid gaps during fast mouse movement. Coordinates are clamped to `[radius, dim-radius]` before drawing to prevent out-of-bounds writes.

## Dependencies

- raylib 5.5 (`brew install raylib`)
- sqlite3 (`brew install sqlite`)
- macOS frameworks in Makefile: `IOKit Cocoa OpenGL CoreVideo`
