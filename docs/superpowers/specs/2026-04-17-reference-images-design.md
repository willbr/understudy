# Reference Images — Design

## Goal

Let the user drop or paste raster images onto the canvas as reference material. Each image can be moved, resized, rotated, given an opacity, and placed either above or below the paint strokes. Images persist per-painting in SQLite.

## User stories

- Drop a PNG/JPG onto the window → it appears centered on the canvas and can be drawn over.
- Cmd/Ctrl+V an image from the clipboard → same behaviour as drop.
- Click an image → selected, with corner resize handles, a rotation handle, and a floating control panel.
- Drag body → move. Drag corner → uniform resize. Drag rotation handle → rotate.
- Floating panel → opacity slider, Above/Below strokes toggle, Delete button.
- Delete key or panel button → remove selected image.
- Escape or click on empty canvas → deselect.
- Save painting → images persist. Reopen → images come back with position, scale, rotation, opacity, and layer.

## Non-goals (v1)

- Per-corner non-uniform scaling.
- Cropping.
- Z-order reordering after drop (always insertion order).
- Copy/cut of reference images back to the system clipboard.
- HEIC or other formats raylib can't decode out of the box.

## Architecture

### New module: `src/refimage.h` (STB-style single header)

Header-only. Interface is always visible; implementation compiles into exactly one translation unit (main.c) via `#define REFIMAGE_IMPLEMENTATION` before the include. No new Makefile entry.

```c
typedef struct {
    int   db_id;                // -1 until persisted
    Texture2D tex;              // GPU handle
    unsigned char *png_bytes;   // raw file bytes, used as DB blob
    int   png_len;
    int   src_w, src_h;         // original pixel dims
    float x, y;                 // center in canvas-local coords
    float scale;                // uniform (preserves aspect)
    float rotation;             // radians
    float opacity;              // 0..1
    bool  above_strokes;
} RefImage;

void refimage_init(void);
void refimage_shutdown(void);
void refimage_clear(void);                             // on painting new/load
void refimage_add(const unsigned char *png, int len, Image decoded); // takes ownership
bool refimage_update(Rectangle canvas_rect);           // true if input consumed
void refimage_draw_below(Rectangle canvas_rect);
void refimage_draw_above(Rectangle canvas_rect);       // incl. handles + panel
int  refimage_count(void);
RefImage *refimage_get(int i);                         // mutable; for db save
void refimage_load_from_db(RefImage *arr, int n);      // takes ownership
```

Internal state: a dynamic array of `RefImage`, a selection index (`-1` = none), and an interaction state machine (`NONE | MOVING | SCALING | ROTATING | PANEL_SLIDER`).

### Canvas integration

`canvas.c` already renders paper + stroke RT in a single `canvas_draw()`. To let ref images be sandwiched between paper and strokes, split into two calls:

- `canvas_draw_background(Rectangle canvas_rect)` — paper + paper shader.
- `canvas_draw_strokes(Rectangle canvas_rect)` — the stroke RT.

`main.c` orchestrates:

```
canvas_draw_background(canvas_rect);
refimage_draw_below(canvas_rect);
canvas_draw_strokes(canvas_rect);
refimage_draw_above(canvas_rect);     // handles + panel layered last
toolbar_draw();
ui_draw();
```

### Input flow

```
if (ui.mode != UI_NONE) ui_update();
else if (IsFileDropped()) refimage_accept_drop();
else if (paste_combo_pressed) refimage_accept_paste();
else {
    bool consumed = refimage_update(canvas_rect);
    if (!consumed) {
        toolbar_update();
        canvas_update();
    }
}
```

`refimage_update` consumes input when:
- Mouse is over the floating panel rect while a selection exists.
- Mouse is over a handle (corner square or rotation circle) of the selected image.
- Mouse is inside any image's bounding box and `IsMouseButtonPressed(LEFT)` (selection / drag start).
- Delete/Backspace with a selection.
- Escape with a selection.

It explicitly does **not** consume when the user clicks on empty canvas area — that click deselects and then falls through to canvas drawing on the **next** frame (not the same frame, to avoid starting a stroke on the deselect click).

## Interaction details

### Selection visuals

- Dashed rectangle outline at the rotated image edges (1 px, 50% black).
- 4 corner handles — filled squares, ~10 px, white with 1 px black border.
- 1 rotation handle — circle, ~12 px diameter, positioned 24 px outside the top edge midpoint along the up-axis of the rotated image.

### Transforms

- **Move**: drag anywhere inside the image bounds (but not on a handle). Updates `x, y`.
- **Scale**: drag any corner. Scale is uniform — measured as `|cursor - center| / |corner_at_scale_1 - center|` relative to the handle grabbed, clamped to `[0.05, 20.0]`.
- **Rotate**: drag rotation handle. Rotation tracks `atan2(cursor - center)` offset from the grab angle.

Shift-modifier snapping is out of scope for v1.

### Floating panel

- Width ~220 px, height ~80 px.
- Anchored 12 px below the rotated bounding box (AABB in screen space). If it would go off-canvas, anchor above instead. Final position clamped to canvas rect.
- Contents, top to bottom:
  - Opacity slider with numeric readout, e.g. `Opacity   [▒▒▒▒▒░░░] 62%`.
  - Two-segment pill: `[ Above ] [ Below ]`.
  - Trash icon button right-aligned.
- Panel rect consumes clicks so dragging the slider doesn't drag the image.

## Persistence

### Schema (migration on open)

```sql
CREATE TABLE IF NOT EXISTS ref_images(
    id            INTEGER PRIMARY KEY,
    painting_id   INTEGER NOT NULL,
    image_idx     INTEGER NOT NULL,
    x             REAL    NOT NULL,
    y             REAL    NOT NULL,
    scale         REAL    NOT NULL,
    rotation      REAL    NOT NULL,
    opacity       REAL    NOT NULL,
    above_strokes INTEGER NOT NULL,
    png_blob      BLOB    NOT NULL,
    FOREIGN KEY(painting_id) REFERENCES paintings(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_ref_images_painting ON ref_images(painting_id, image_idx);
```

### db.c surface

```c
int  db_save_ref_images(sqlite3 *db, int painting_id,
                        const RefImage *images, int count);
int  db_load_ref_images(sqlite3 *db, int painting_id,
                        RefImage **out, int *out_count);
void db_free_ref_images(RefImage *images, int count);
```

- Save path: `DELETE FROM ref_images WHERE painting_id = ?` then insert each row. Mirrors the stroke-save pattern. `sqlite3_bind_blob(..., SQLITE_TRANSIENT)` so SQLite copies the PNG bytes.
- Load path: allocates a `RefImage[]`, copies PNG bytes out of the blob column, decodes `Image` via `LoadImageFromMemory(".png", ..., len)` so it works regardless of original extension, uploads `Texture2D`. Returned array is owned by caller.
- `db_free_ref_images` frees `png_bytes`, unloads textures, frees the array. Used by `refimage_clear` and on shutdown.

## Drop and paste

### Drop

```c
if (IsFileDropped()) {
    FilePathList list = LoadDroppedFiles();
    for (unsigned i = 0; i < list.count; i++) {
        int len;
        unsigned char *bytes = LoadFileData(list.paths[i], &len);
        Image img = LoadImage(list.paths[i]);
        if (bytes && img.data) {
            refimage_add(bytes, len, img);  // takes ownership of both
        } else {
            if (bytes) UnloadFileData(bytes);
            if (img.data) UnloadImage(img);
        }
    }
    UnloadDroppedFiles(list);
}
```

Files that raylib can't decode are silently ignored (no modal error in v1).

### Paste

```c
bool cmd = IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER)
        || IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
if (cmd && IsKeyPressed(KEY_V)) {
    Image img = GetClipboardImage();
    if (img.data) {
        int len;
        unsigned char *png = ExportImageToMemory(img, ".png", &len);
        refimage_add(png, len, img);    // takes ownership
    }
}
```

Clipboard images arrive as decoded pixels, so we re-encode to PNG for the DB blob. Lossless for the PNG round-trip.

### Placement defaults

- Center: canvas center.
- Scale: `min(1.0, 0.6 * canvas_w / src_w, 0.6 * canvas_h / src_h)` — new images fit comfortably without covering the whole canvas.
- Rotation: 0.
- Opacity: 1.0.
- `above_strokes`: true.
- `db_id`: -1.

## Memory and lifecycle

- `refimage_add(const unsigned char *png, int len, Image decoded)` copies the PNG bytes into a fresh `malloc`ed buffer and uploads the decoded `Image` as a `Texture2D`. The caller retains ownership of both inputs and is responsible for freeing them (drop path: `UnloadFileData` + `UnloadImage`; paste path: `MemFree` + `UnloadImage`). This keeps the module's ownership rules uniform regardless of source.
- `refimage_load_from_db` takes ownership of a `RefImage[]` whose `png_bytes` were `malloc`ed by the DB layer — same allocator as `refimage_add` uses internally, so the cleanup path is identical.
- `refimage_clear` (called on New and on Load) `free`s every entry's `png_bytes`, `UnloadTexture(tex)`, resets the array, clears selection.
- `refimage_shutdown` calls `refimage_clear` and frees the array backing memory.

## Files touched

- **new** `src/refimage.h` — header-only module.
- `src/main.c`:
  - `#define REFIMAGE_IMPLEMENTATION` once before the include.
  - Route drop, paste, update, and draw calls.
  - Call `refimage_clear` on new painting / load painting.
  - Call `db_save_ref_images` alongside existing stroke save.
  - Call `db_load_ref_images` + `refimage_load_from_db` on load.
- `src/canvas.c` / `canvas.h`:
  - Split `canvas_draw` into `canvas_draw_background` and `canvas_draw_strokes`.
- `src/db.c` / `db.h`:
  - Add `CREATE TABLE IF NOT EXISTS ref_images` + index to `db_init`.
  - Add `db_save_ref_images`, `db_load_ref_images`, `db_free_ref_images`.
- `Makefile`: no changes.

## Risks and open questions

- **Rotation hit-testing**: inside-image and handle hit-tests must work in the rotated frame. Implementation will transform the cursor into image-local space once per frame when a selection exists.
- **Paste while toolbar has focus**: the toolbar doesn't currently capture Cmd/Ctrl+V, so routing paste at the top of the input chain is safe. Revisit if toolbar gains text input.
- **Very large images** (e.g. 8K): uploaded as a single `Texture2D`. raylib clamps to `MAX_TEXTURE_SIZE`; oversize images are silently downscaled by raylib. Acceptable for v1.
- **Format support**: raylib decodes PNG, BMP, TGA, JPG, GIF (static), PSD (limited), HDR, PIC, PPM, PGM, DDS, KTX, PVR, ASTC. No HEIC. Good enough for v1.
