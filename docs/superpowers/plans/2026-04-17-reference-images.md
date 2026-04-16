# Reference Images Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drop or paste raster images onto the canvas as reference overlays. Each image can be moved, resized, rotated, given an opacity, and placed above or below the paint strokes. Persisted per-painting in SQLite.

**Architecture:** A new single-header module `refimage.h` (STB-style, `#define REFIMAGE_IMPLEMENTATION` in main.c) owns a dynamic array of `RefImage` structs in document-space coordinates. The existing canvas pipeline is refactored once so the main render texture contains strokes-only (transparent background); paper is rendered as a separate layer. Ref images sit between paper and strokes (below mode) or after strokes (above mode). Persistence adds a `ref_images` table with PNG blobs and transform state.

**Tech Stack:** C11, raylib 5.5, SQLite3, OpenGL 3.30 (fragment shader), GLSL.

**Spec:** [2026-04-17-reference-images-design.md](../specs/2026-04-17-reference-images-design.md)

**Testing note:** This is a raylib GUI app with no test harness. Each task that produces a behavioural change ends with a manual verification step (`make run`, specific actions to perform, expected outcome). Each task ends with a commit.

**Context for the engineer:**

- `src/main.c` is the giant input-routing and render-orchestration loop. Input flows through the "else" branch starting near line 100, under `if (app.ui.mode == UI_NONE)`.
- `src/canvas.c` holds a `Canvas` struct with `strokes_rt` (scratch), `committed_rt` (cache of all layers composited), `rt` (final with paper baked in via `ink.fs`), and `paper_tex` (used only for minimap).
- `ink.fs` is the fragment shader that composites strokes over procedurally-generated paper. It produces opaque pixels.
- View transform: `canvas.view_x`, `canvas.view_y`, `canvas.zoom`. Document coordinates map to screen coordinates as `(doc * zoom) + view + panel_offset`.
- Canvas panel starts at x = `canvas_x` (either `0` if toolbar hidden, else `CANVAS_X = 220`) and y = `CANVAS_Y = 0`. Width is `GetScreenWidth() - canvas_x`, height is `GetScreenHeight()`.
- Input gate: all new input handlers go inside `if (app.ui.mode == UI_NONE)` in main.c. Paste (Cmd/Ctrl+V) must be before toolbar_update so it isn't intercepted.
- Autosave currently fires on stroke-end. Ref image manipulation will also trigger autosave on mouse-release.

---

## File Structure

**Create:**
- `src/refimage.h` — single-header STB-style module. Public API + private impl behind `REFIMAGE_IMPLEMENTATION`.

**Modify:**
- `src/ink.fs` — output transparent outside the doc and where no stroke; remove paper show-through.
- `src/canvas.h` — expose `paper_rt`; add `canvas_draw_paper()`, `canvas_draw_strokes()`, `canvas_draw_dark_bg()` (split of `canvas_draw`). Remove `canvas_draw`.
- `src/canvas.c` — render paper into `paper_rt` on view change; update `composite_ink` to emit transparent where no stroke; split `canvas_draw` into three calls.
- `src/db.h` — add `db_save_ref_images`, `db_load_ref_images`, `db_free_ref_images`, plus a forward-declared `RefImage` type (defined in refimage.h).
- `src/db.c` — add `ref_images` table to schema, bump `SCHEMA_VERSION`, implement the three functions.
- `src/main.c` — `#define REFIMAGE_IMPLEMENTATION` before include; drop/paste handlers; refimage_update call; new canvas_draw_* orchestration; save/load/clear hooks; autosave on manipulation end.

**No changes:** `Makefile`, `tools.*`, `toolbar.*`, `ui.*`, `paper.fs`, `font.h`.

---

## Phase 1: Canvas pipeline refactor

Before ref images can sit "below strokes," the strokes RT must have a transparent background. Currently `ink.fs` bakes paper into every pixel. This phase separates paper from strokes.

### Task 1: Make ink.fs output transparent outside strokes

**Files:**
- Modify: `src/ink.fs`

- [ ] **Step 1: Replace the shader body**

Overwrite `src/ink.fs` entirely with:

```glsl
#version 330

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;   // strokes RT (auto-bound by raylib)
uniform vec2 docSize;
uniform vec2 viewOffset;
uniform float zoom;
uniform vec2 resolution;

float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    vec2 shift = vec2(100.0);
    for (int i = 0; i < 5; i++) {
        v += amp * noise(p);
        p = p * 2.0 + shift;
        amp *= 0.5;
    }
    return v;
}

void main() {
    vec4 stroke = texture(texture0, fragTexCoord);

    // No ink → fully transparent (lets whatever is under this RT show through)
    if (stroke.a < 0.01) {
        finalColor = vec4(0.0);
        return;
    }

    // Document-space coordinate (for grain lookup only)
    vec2 screenPos = vec2(gl_FragCoord.x, resolution.y - gl_FragCoord.y);
    vec2 docCoord  = (screenPos - viewOffset) / zoom;

    // Edge detection: neighbour alpha to find stroke boundary
    vec2 px = 1.0 / resolution;
    float nl = texture(texture0, fragTexCoord + vec2(-px.x, 0.0)).a;
    float nr = texture(texture0, fragTexCoord + vec2( px.x, 0.0)).a;
    float nt = texture(texture0, fragTexCoord + vec2(0.0,  px.y)).a;
    float nb = texture(texture0, fragTexCoord + vec2(0.0, -px.y)).a;
    float minN = min(min(nl, nr), min(nt, nb));
    float isEdge = 1.0 - smoothstep(0.0, 0.4, minN / max(stroke.a, 0.001));

    // Paper grain erodes the stroke edge for a watercolor look
    float grain = fbm(docCoord / 6.0 + vec2(7.3, 2.8));
    float edgeErosion = smoothstep(0.3, 0.7, grain) * isEdge;

    // Pigment pooling: subtle concentration variation
    float pooling = noise(docCoord / 20.0 + vec2(42.0, 17.0));
    float concentrate = mix(0.95, 1.05, pooling);
    vec3 inkColor = stroke.rgb * concentrate;

    // Output ink with alpha reduced at edges — background shows through at edges
    float alpha = (1.0 - edgeErosion * 0.6) * stroke.a;
    finalColor = vec4(inkColor, alpha);
}
```

Key differences from before: outputs transparent (not paper) where `stroke.a < 0.01`; outputs `vec4(inkColor, alpha)` with per-pixel alpha (not mix with paper); no paper show-through effect in stroke body (paper is no longer accessible from this shader).

- [ ] **Step 2: Build**

Run: `make`

Expected: builds with no errors.

- [ ] **Step 3: Commit**

```bash
git add src/ink.fs
git commit -m "ink shader: emit transparent outside strokes for layered compositing"
```

### Task 2: Add `paper_rt` to Canvas and render paper into it

**Files:**
- Modify: `src/canvas.h` — add field and function declaration
- Modify: `src/canvas.c` — lifecycle, rendering

- [ ] **Step 1: Add the field to `Canvas` struct**

In `src/canvas.h`, inside the `typedef struct Canvas` block (after `paper_tex` around line 42), add:

```c
    RenderTexture2D paper_rt;     // paper rendered at current view transform
    Shader          paper_shader; // paper.fs — used for paper_rt
```

- [ ] **Step 2: Declare new draw helpers and remove old `canvas_draw`**

Edit `src/canvas.h`. Replace the `canvas_draw` declaration with:

```c
// Background fill (dark gray) for area outside document
void canvas_draw_dark_bg(const Canvas *c, int x_offset);

// Paper layer (below strokes, above dark bg)
void canvas_draw_paper(const Canvas *c, int x_offset);

// Strokes layer (transparent where no ink)
void canvas_draw_strokes(const Canvas *c, int x_offset);

// Document border (drawn on top of everything canvas-related)
void canvas_draw_border(const Canvas *c, int x_offset);
```

- [ ] **Step 3: Load the paper shader in `canvas_init`**

Find `canvas_init` in `src/canvas.c`. Near the existing `c->ink_shader = LoadShader(0, "src/ink.fs");` line, add:

```c
    c->paper_shader = LoadShader(0, "src/paper.fs");
    c->paper_rt     = LoadRenderTexture(CANVAS_WIDTH, CANVAS_HEIGHT);
```

- [ ] **Step 4: Free them in `canvas_free`**

In `canvas_free`, near the existing `UnloadShader(c->ink_shader);`:

```c
    UnloadShader(c->paper_shader);
    UnloadRenderTexture(c->paper_rt);
```

- [ ] **Step 5: Add `canvas_resize` handling**

Find `canvas_resize` in `src/canvas.c`. Where it reallocates `c->rt` and `c->strokes_rt`, add the same for `paper_rt`:

```c
    UnloadRenderTexture(c->paper_rt);
    c->paper_rt = LoadRenderTexture(panel_w, panel_h);
```

- [ ] **Step 6: Add `render_paper` static helper**

Add this static function in `src/canvas.c` before `composite_ink`:

```c
static void render_paper(Canvas *c) {
    int pw = c->paper_rt.texture.width;
    int ph = c->paper_rt.texture.height;

    BeginTextureMode(c->paper_rt);
    ClearBackground(BLANK);

    float docSize[2] = {(float)c->width, (float)c->height};
    float viewOff[2] = {c->view_x, c->view_y};
    float res[2]     = {(float)pw, (float)ph};
    SetShaderValue(c->paper_shader,
                   GetShaderLocation(c->paper_shader, "docSize"),
                   docSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(c->paper_shader,
                   GetShaderLocation(c->paper_shader, "viewOffset"),
                   viewOff, SHADER_UNIFORM_VEC2);
    SetShaderValue(c->paper_shader,
                   GetShaderLocation(c->paper_shader, "zoom"),
                   &c->zoom, SHADER_UNIFORM_FLOAT);
    SetShaderValue(c->paper_shader,
                   GetShaderLocation(c->paper_shader, "resolution"),
                   res, SHADER_UNIFORM_VEC2);

    // Paper only renders inside the document rect in screen space.
    BeginShaderMode(c->paper_shader);
    float sx = c->view_x;
    float sy = c->view_y;
    float sw = (float)c->width  * c->zoom;
    float sh = (float)c->height * c->zoom;
    DrawRectangle((int)sx, (int)sy, (int)sw, (int)sh, WHITE);
    EndShaderMode();

    EndTextureMode();
}
```

Note: `paper.fs` as written uses `fragTexCoord * docSize` — that won't align with view transform. We need to update `paper.fs` to match the view-transform style. Do that in the next step.

- [ ] **Step 7: Update `paper.fs` to use view-transform uniforms**

Overwrite `src/paper.fs` with:

```glsl
#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform vec2 docSize;
uniform vec2 viewOffset;
uniform float zoom;
uniform vec2 resolution;

out vec4 finalColor;

float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    vec2 shift = vec2(100.0);
    for (int i = 0; i < 5; i++) {
        v += amp * noise(p);
        p = p * 2.0 + shift;
        amp *= 0.5;
    }
    return v;
}

void main() {
    // Document-space coordinate derived from screen-space fragment position
    vec2 screenPos = vec2(gl_FragCoord.x, resolution.y - gl_FragCoord.y);
    vec2 docCoord  = (screenPos - viewOffset) / zoom;

    if (docCoord.x < 0.0 || docCoord.x > docSize.x ||
        docCoord.y < 0.0 || docCoord.y > docSize.y) {
        finalColor = vec4(0.0);
        return;
    }

    vec2 uv = docCoord / 8.0;
    float fibers = fbm(vec2(uv.x * 0.3, uv.y * 0.6) + vec2(3.7, 1.2));
    float bumps  = fbm(uv * 0.8 + vec2(7.3, 2.8));
    float grain  = noise(uv * 2.0 + vec2(13.1, 5.5));
    float paper  = fibers * 0.45 + bumps * 0.35 + grain * 0.2;
    float base   = 0.92;
    float variation = 0.12;
    float v = base + (paper - 0.5) * variation;
    v = clamp(v, 0.82, 0.98);

    vec3 color = vec3(v, v * 0.995, v * 0.97);
    finalColor = vec4(color, 1.0);
}
```

Note: `draw_paper_bitmap` (used for minimap) still uses `paper_tex` — unchanged, so minimap paper rendering keeps working.

- [ ] **Step 8: Call `render_paper` wherever `composite_ink` is currently called**

Find every call site of `composite_ink` in `src/canvas.c` (there are several, including `canvas_redraw_for_view`, end-of-stroke, etc.). Grep first:

Run: `grep -n "composite_ink" src/canvas.c`

At each call site, add `render_paper(c);` on the line immediately before `composite_ink(c);`. Exception: calls that are inside a per-stroke incremental render loop (not inside view-change redraws) don't need it — paper doesn't change between strokes. Only add `render_paper` to:

- `canvas_redraw_for_view`
- `canvas_resize`
- `canvas_init` (after the initial redraw)

Add `render_paper(c);` to these once, alongside the existing `composite_ink(c);` or `redraw_all(c);`.

- [ ] **Step 9: Implement the four new draw helpers**

Replace the existing `canvas_draw` function body with these four functions:

```c
void canvas_draw_dark_bg(const Canvas *c, int x_offset) {
    int pw = c->rt.texture.width;
    int ph = c->rt.texture.height;
    DrawRectangle(x_offset, CANVAS_Y, pw, ph, (Color){45, 45, 45, 255});
}

void canvas_draw_paper(const Canvas *c, int x_offset) {
    int pw = c->paper_rt.texture.width;
    int ph = c->paper_rt.texture.height;
    Rectangle src  = {0, 0, (float)pw, -(float)ph};
    Rectangle dest = {(float)x_offset, CANVAS_Y, (float)pw, (float)ph};
    DrawTexturePro(c->paper_rt.texture, src, dest, (Vector2){0, 0}, 0.0f, WHITE);
}

void canvas_draw_strokes(const Canvas *c, int x_offset) {
    int pw = c->rt.texture.width;
    int ph = c->rt.texture.height;
    Rectangle src  = {0, 0, (float)pw, -(float)ph};
    Rectangle dest = {(float)x_offset, CANVAS_Y, (float)pw, (float)ph};
    DrawTexturePro(c->rt.texture, src, dest, (Vector2){0, 0}, 0.0f, WHITE);
}

void canvas_draw_border(const Canvas *c, int x_offset) {
    int pw = c->rt.texture.width;
    int ph = c->rt.texture.height;
    float bx = (float)x_offset + c->view_x;
    float by = CANVAS_Y + c->view_y;
    float bw = (float)c->width  * c->zoom;
    float bh = (float)c->height * c->zoom;
    BeginScissorMode(x_offset, CANVAS_Y, pw, ph);
    DrawRectangleLinesEx((Rectangle){bx, by, bw, bh}, 1.0f,
                         (Color){90, 90, 90, 180});
    EndScissorMode();
}
```

- [ ] **Step 10: Also update `composite_ink` in canvas.c**

Since the shader no longer produces paper pixels, `composite_ink` should clear `rt` to transparent (not dark gray) so we see paper through it.

In `composite_ink` (`src/canvas.c`), change:

```c
    ClearBackground((Color){45, 45, 45, 255});
```

to:

```c
    ClearBackground(BLANK);
```

- [ ] **Step 11: Replace `canvas_draw` call in main.c**

In `src/main.c`, find `canvas_draw(&app.canvas, canvas_x);` (near line 458). Replace with:

```c
            canvas_draw_dark_bg(&app.canvas, canvas_x);
            canvas_draw_paper(&app.canvas, canvas_x);
            canvas_draw_strokes(&app.canvas, canvas_x);
            canvas_draw_border(&app.canvas, canvas_x);
```

- [ ] **Step 12: Build**

Run: `make`

Expected: builds with no errors.

- [ ] **Step 13: Manual verification**

Run: `make run`

Do:
- Draw several strokes on the canvas with default colors.
- Change zoom (scroll wheel or press `3`, `2`, `1`).
- Pan with Space+drag.

Expected: canvas looks identical to before — paper texture visible, watercolor strokes render normally, zoom/pan work. If strokes appear on dark gray instead of paper, `render_paper` isn't being called or paper_rt isn't drawing. Fix before committing.

- [ ] **Step 14: Commit**

```bash
git add src/canvas.h src/canvas.c src/ink.fs src/paper.fs src/main.c
git commit -m "canvas: split paper/strokes into separate RTs for layered compositing"
```

---

## Phase 2: refimage module skeleton

### Task 3: Create refimage.h with types and lifecycle

**Files:**
- Create: `src/refimage.h`
- Modify: `src/main.c` — `#define REFIMAGE_IMPLEMENTATION` + include + init/shutdown

- [ ] **Step 1: Create `src/refimage.h`**

```c
#pragma once

#include <stdbool.h>
#include "raylib.h"

// A single raster reference image overlaid on the canvas.
// Coordinates are in document space (0..CANVAS_DOC_W).
typedef struct RefImage {
    int   db_id;                // -1 until persisted
    Texture2D tex;              // GPU handle
    unsigned char *png_bytes;   // malloc'd copy of PNG bytes, for DB blob
    int   png_len;
    int   src_w, src_h;         // original pixel dims
    float x, y;                 // center in document-space coords
    float scale;                // uniform (preserves aspect)
    float rotation;             // radians
    float opacity;              // 0..1
    bool  above_strokes;
} RefImage;

void refimage_init(void);
void refimage_shutdown(void);
void refimage_clear(void);                    // on painting new/load

// Adds a new image. Copies png bytes internally; uploads decoded.
// Caller retains ownership of both inputs.
void refimage_add(const unsigned char *png, int png_len, Image decoded);

// Replace the in-memory list with images loaded from DB.
// Takes ownership of `arr` and its png_bytes (each entry must use malloc'd bytes).
void refimage_load_from_db(RefImage *arr, int n);

int  refimage_count(void);
RefImage *refimage_get(int i);                // mutable; used by db save

#ifdef REFIMAGE_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

typedef struct {
    RefImage *items;
    int count;
    int capacity;
    int selected;        // -1 = none
} RefImageList;

static RefImageList g_refs;

void refimage_init(void) {
    g_refs.items = NULL;
    g_refs.count = 0;
    g_refs.capacity = 0;
    g_refs.selected = -1;
}

static void free_item(RefImage *r) {
    if (r->tex.id) UnloadTexture(r->tex);
    free(r->png_bytes);
    r->tex = (Texture2D){0};
    r->png_bytes = NULL;
    r->png_len = 0;
}

void refimage_clear(void) {
    for (int i = 0; i < g_refs.count; i++) free_item(&g_refs.items[i]);
    g_refs.count = 0;
    g_refs.selected = -1;
}

void refimage_shutdown(void) {
    refimage_clear();
    free(g_refs.items);
    g_refs.items = NULL;
    g_refs.capacity = 0;
}

int refimage_count(void) { return g_refs.count; }

RefImage *refimage_get(int i) {
    if (i < 0 || i >= g_refs.count) return NULL;
    return &g_refs.items[i];
}

static void ensure_capacity(int need) {
    if (need <= g_refs.capacity) return;
    int new_cap = g_refs.capacity ? g_refs.capacity * 2 : 4;
    while (new_cap < need) new_cap *= 2;
    g_refs.items = realloc(g_refs.items, new_cap * sizeof(RefImage));
    g_refs.capacity = new_cap;
}

void refimage_add(const unsigned char *png, int png_len, Image decoded) {
    ensure_capacity(g_refs.count + 1);
    RefImage *r = &g_refs.items[g_refs.count++];
    r->db_id = -1;
    r->tex = LoadTextureFromImage(decoded);
    r->png_bytes = malloc(png_len);
    memcpy(r->png_bytes, png, png_len);
    r->png_len = png_len;
    r->src_w = decoded.width;
    r->src_h = decoded.height;
    r->x = 0; r->y = 0;              // caller should set sensible defaults
    r->scale = 1.0f;
    r->rotation = 0.0f;
    r->opacity = 1.0f;
    r->above_strokes = true;
}

void refimage_load_from_db(RefImage *arr, int n) {
    refimage_clear();
    ensure_capacity(n);
    memcpy(g_refs.items, arr, n * sizeof(RefImage));
    g_refs.count = n;
    // caller gave us ownership of png_bytes; textures were already uploaded
    free(arr);  // the array itself, not the items
}

#endif // REFIMAGE_IMPLEMENTATION
```

- [ ] **Step 2: Include in main.c with impl macro**

In `src/main.c`, at the top near the other includes, after `#include "font.h"`:

```c
#define REFIMAGE_IMPLEMENTATION
#include "refimage.h"
```

- [ ] **Step 3: Call init and shutdown**

In `src/main.c`, after `ui_init(&app.ui);` (around line 47), add:

```c
    refimage_init();
```

At the end of main, near `ui_free(&app.ui);`:

```c
    refimage_shutdown();
```

- [ ] **Step 4: Build**

Run: `make`

Expected: builds with no warnings related to refimage.h (might get unused-function warnings — acceptable at this stage).

- [ ] **Step 5: Commit**

```bash
git add src/refimage.h src/main.c
git commit -m "add refimage module skeleton with list lifecycle"
```

---

## Phase 3: Drop & paste input

### Task 4: Wire file drop to refimage_add with sensible defaults

**Files:**
- Modify: `src/refimage.h` — add helper for default placement
- Modify: `src/main.c` — drop handler

- [ ] **Step 1: Add a placement helper in refimage.h**

Inside the `#ifdef REFIMAGE_IMPLEMENTATION` block, before the final `#endif`, add:

```c
// Set the newly-added (last) image's defaults given canvas doc dims.
void refimage_set_defaults(float doc_w, float doc_h) {
    if (g_refs.count == 0) return;
    RefImage *r = &g_refs.items[g_refs.count - 1];
    r->x = doc_w * 0.5f;
    r->y = doc_h * 0.5f;
    float fit_w = (doc_w * 0.6f) / (float)r->src_w;
    float fit_h = (doc_h * 0.6f) / (float)r->src_h;
    float fit = fmin(fit_w, fit_h);
    r->scale = (fit < 1.0f) ? fit : 1.0f;
}
```

And above the `#ifdef REFIMAGE_IMPLEMENTATION`, in the public API block, declare:

```c
void refimage_set_defaults(float doc_w, float doc_h);
```

- [ ] **Step 2: Add drop handler in main.c**

In `src/main.c`, inside the `if (app.ui.mode == UI_NONE) { ... }` block, near the top (before `toolbar_update`), add:

```c
            if (IsFileDropped()) {
                FilePathList list = LoadDroppedFiles();
                for (unsigned i = 0; i < list.count; i++) {
                    int len = 0;
                    unsigned char *bytes = LoadFileData(list.paths[i], &len);
                    Image img = LoadImage(list.paths[i]);
                    if (bytes && img.data && len > 0) {
                        refimage_add(bytes, len, img);
                        refimage_set_defaults((float)app.canvas.width,
                                              (float)app.canvas.height);
                        app.canvas.dirty = true;
                    }
                    if (bytes) UnloadFileData(bytes);
                    if (img.data) UnloadImage(img);
                }
                UnloadDroppedFiles(list);
            }
```

- [ ] **Step 3: Build**

Run: `make`

Expected: builds with no errors.

- [ ] **Step 4: Manual verification**

Run: `make run`

Do:
- Drag a PNG or JPG file from Finder onto the app window.

Expected: no visible change yet (we haven't wired drawing), but no crash. We'll verify the image list via a temporary log in the next task if needed.

- [ ] **Step 5: Commit**

```bash
git add src/refimage.h src/main.c
git commit -m "refimage: accept file drops, set centered defaults"
```

### Task 5: Wire clipboard paste (Cmd/Ctrl+V)

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Add paste handler in main.c**

In `src/main.c`, just after the drop handler from Task 4, add:

```c
            {
                bool cmd = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) ||
                           IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                if (cmd && IsKeyPressed(KEY_V)) {
                    Image img = GetClipboardImage();
                    if (img.data) {
                        int len = 0;
                        unsigned char *png = ExportImageToMemory(img, ".png", &len);
                        if (png && len > 0) {
                            refimage_add(png, len, img);
                            refimage_set_defaults((float)app.canvas.width,
                                                  (float)app.canvas.height);
                            app.canvas.dirty = true;
                        }
                        if (png) MemFree(png);
                        UnloadImage(img);
                    }
                }
            }
```

- [ ] **Step 2: Build**

Run: `make`

Expected: builds. If `GetClipboardImage` or `ExportImageToMemory` is missing, ensure raylib is 5.5+ (`pkg-config --modversion raylib`).

- [ ] **Step 3: Manual verification** — defer until Task 6 when drawing is wired.

- [ ] **Step 4: Commit**

```bash
git add src/main.c
git commit -m "refimage: accept clipboard paste via Cmd/Ctrl+V"
```

---

## Phase 4: Drawing ref images

### Task 6: Draw ref images at doc-space positions in main.c

**Files:**
- Modify: `src/refimage.h` — implement draw functions
- Modify: `src/main.c` — call draw at the right point in render order

- [ ] **Step 1: Add draw functions to refimage.h public API**

In the public API block (before `#ifdef REFIMAGE_IMPLEMENTATION`), add:

```c
// Draw all images with above_strokes == want_above.
// All args describe the canvas viewport so images align with doc coords.
void refimage_draw(bool want_above,
                   int canvas_x, int canvas_y,
                   float view_x, float view_y, float zoom,
                   int panel_w, int panel_h);
```

- [ ] **Step 2: Implement inside REFIMAGE_IMPLEMENTATION block**

Add before the final `#endif`:

```c
void refimage_draw(bool want_above,
                   int canvas_x, int canvas_y,
                   float view_x, float view_y, float zoom,
                   int panel_w, int panel_h) {
    BeginScissorMode(canvas_x, canvas_y, panel_w, panel_h);
    for (int i = 0; i < g_refs.count; i++) {
        RefImage *r = &g_refs.items[i];
        if (r->above_strokes != want_above) continue;

        float sx = canvas_x + view_x + r->x * zoom;
        float sy = canvas_y + view_y + r->y * zoom;
        float sw = (float)r->src_w * r->scale * zoom;
        float sh = (float)r->src_h * r->scale * zoom;

        Rectangle src  = {0, 0, (float)r->src_w, (float)r->src_h};
        Rectangle dest = {sx, sy, sw, sh};
        Vector2 origin = {sw * 0.5f, sh * 0.5f};
        Color tint = {255, 255, 255, (unsigned char)(r->opacity * 255.0f)};
        DrawTexturePro(r->tex, src, dest, origin,
                       r->rotation * RAD2DEG, tint);
    }
    EndScissorMode();
}
```

- [ ] **Step 3: Call in main.c between canvas layers**

In `src/main.c`, find the block from Task 1 step 11:

```c
            canvas_draw_dark_bg(&app.canvas, canvas_x);
            canvas_draw_paper(&app.canvas, canvas_x);
            canvas_draw_strokes(&app.canvas, canvas_x);
            canvas_draw_border(&app.canvas, canvas_x);
```

Replace with:

```c
            canvas_draw_dark_bg(&app.canvas, canvas_x);
            canvas_draw_paper(&app.canvas, canvas_x);
            refimage_draw(false,                                           // below
                          canvas_x, CANVAS_Y,
                          app.canvas.view_x, app.canvas.view_y, app.canvas.zoom,
                          app.canvas.rt.texture.width, app.canvas.rt.texture.height);
            canvas_draw_strokes(&app.canvas, canvas_x);
            refimage_draw(true,                                            // above
                          canvas_x, CANVAS_Y,
                          app.canvas.view_x, app.canvas.view_y, app.canvas.zoom,
                          app.canvas.rt.texture.width, app.canvas.rt.texture.height);
            canvas_draw_border(&app.canvas, canvas_x);
```

- [ ] **Step 4: Build**

Run: `make`

Expected: builds with no errors.

- [ ] **Step 5: Manual verification**

Run: `make run`

Do:
- Drop an image file from Finder onto the window.
- Press `3` to reset zoom, `2` to zoom out, `1` to zoom further.
- Pan with Space+drag.
- Copy an image in another app, then Cmd+V into the paint app.

Expected: image(s) appear centered on the canvas. They pan and zoom with the canvas. Default `above_strokes = true`, so drawn strokes appear under the image. No crash on drop/paste. Opacity is 1.0 so image is fully opaque.

If below/above visual: try toggling `above_strokes` manually in the debugger or tempo hack a line `g_refs.items[0].above_strokes = false;` to verify strokes appear over the image when below. Revert the hack.

- [ ] **Step 6: Commit**

```bash
git add src/refimage.h src/main.c
git commit -m "refimage: render above/below strokes at document-space coords"
```

---

## Phase 5: Selection and move

### Task 7: Add selection state and hit-testing

**Files:**
- Modify: `src/refimage.h` — selection logic + update function skeleton

This task builds just selection and deselection. Move/resize/rotate come next.

- [ ] **Step 1: Add public API declarations**

In `src/refimage.h` public block:

```c
// Returns true if input was consumed (selection happened, handle click, etc.).
// Caller should skip normal canvas input when this returns true.
bool refimage_update(int canvas_x, int canvas_y,
                     float view_x, float view_y, float zoom,
                     int panel_w, int panel_h);

int  refimage_selected(void);         // -1 = none
void refimage_set_selected(int idx);  // clamps; -1 to deselect
```

- [ ] **Step 2: Implement helpers and update()**

Inside REFIMAGE_IMPLEMENTATION block, before `refimage_draw`, add:

```c
#include <math.h>

int refimage_selected(void) { return g_refs.selected; }

void refimage_set_selected(int idx) {
    if (idx < -1 || idx >= g_refs.count) g_refs.selected = -1;
    else g_refs.selected = idx;
}

// Convert screen mouse pos to document-space coord.
static void screen_to_doc(float mx, float my,
                          int canvas_x, int canvas_y,
                          float view_x, float view_y, float zoom,
                          float *out_x, float *out_y) {
    *out_x = (mx - canvas_x - view_x) / zoom;
    *out_y = (my - canvas_y - view_y) / zoom;
}

// Is point (doc-space) inside the rotated rect of image r?
static bool point_in_image(const RefImage *r, float dx, float dy) {
    // Transform dx,dy into image-local coords (unrotated, centered at image center)
    float c = cosf(-r->rotation);
    float s = sinf(-r->rotation);
    float lx = (dx - r->x) * c - (dy - r->y) * s;
    float ly = (dx - r->x) * s + (dy - r->y) * c;
    float hw = r->src_w * r->scale * 0.5f;
    float hh = r->src_h * r->scale * 0.5f;
    return (lx >= -hw && lx <= hw && ly >= -hh && ly <= hh);
}

bool refimage_update(int canvas_x, int canvas_y,
                     float view_x, float view_y, float zoom,
                     int panel_w, int panel_h) {
    (void)panel_w; (void)panel_h;
    Vector2 m = GetMousePosition();
    float dx, dy;
    screen_to_doc(m.x, m.y, canvas_x, canvas_y, view_x, view_y, zoom, &dx, &dy);

    // Escape: deselect
    if (g_refs.selected != -1 && IsKeyPressed(KEY_ESCAPE)) {
        g_refs.selected = -1;
        return true;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        // Hit-test images in reverse order (topmost first)
        int hit = -1;
        for (int i = g_refs.count - 1; i >= 0; i--) {
            if (point_in_image(&g_refs.items[i], dx, dy)) { hit = i; break; }
        }
        if (hit != -1) {
            g_refs.selected = hit;
            return true;
        }
        if (g_refs.selected != -1) {
            g_refs.selected = -1;
            return true;   // consume the deselect click so no stroke starts
        }
    }

    return false;
}
```

- [ ] **Step 3: Add a selection outline to `refimage_draw`**

Modify the `refimage_draw` function to accept a `is_selected` draw path. Simpler: add a new function `refimage_draw_selection_overlay`. In the public block add:

```c
void refimage_draw_selection_overlay(int canvas_x, int canvas_y,
                                     float view_x, float view_y, float zoom);
```

And in the impl block:

```c
void refimage_draw_selection_overlay(int canvas_x, int canvas_y,
                                     float view_x, float view_y, float zoom) {
    if (g_refs.selected < 0 || g_refs.selected >= g_refs.count) return;
    RefImage *r = &g_refs.items[g_refs.selected];

    float hw = r->src_w * r->scale * 0.5f;
    float hh = r->src_h * r->scale * 0.5f;
    float c = cosf(r->rotation);
    float s = sinf(r->rotation);

    Vector2 corners_local[4] = {
        {-hw, -hh}, { hw, -hh}, { hw,  hh}, {-hw,  hh}
    };
    Vector2 corners_screen[4];
    for (int i = 0; i < 4; i++) {
        float lx = corners_local[i].x;
        float ly = corners_local[i].y;
        float dx = r->x + lx * c - ly * s;
        float dy = r->y + lx * s + ly * c;
        corners_screen[i].x = canvas_x + view_x + dx * zoom;
        corners_screen[i].y = canvas_y + view_y + dy * zoom;
    }

    // Dashed-ish outline (just solid thin for v1 — can be dashed later)
    for (int i = 0; i < 4; i++)
        DrawLineEx(corners_screen[i], corners_screen[(i + 1) % 4], 1.5f,
                   (Color){0, 0, 0, 180});
}
```

- [ ] **Step 4: Call update and overlay from main.c**

In `src/main.c`, inside the `if (app.ui.mode == UI_NONE)` block, near the TOP (after drop/paste), add:

```c
            bool refimage_consumed = refimage_update(
                canvas_x, CANVAS_Y,
                app.canvas.view_x, app.canvas.view_y, app.canvas.zoom,
                app.canvas.rt.texture.width, app.canvas.rt.texture.height);
```

Now we need to skip canvas input when refimage consumed. Find the toolbar + canvas input block (the big `else { zoom_rect_active = false; ... }` around line 378 that handles brush/line tool). Wrap its contents with:

```c
            } else if (refimage_consumed) {
                // ref image consumed the input; skip canvas drawing
                zoom_rect_active = false;
                SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
                if (app.canvas.is_drawing)
                    canvas_end_stroke(&app.canvas);
            } else {
                zoom_rect_active = false;
                SetMouseCursor(MOUSE_CURSOR_DEFAULT);
                // ... (existing brush/line tool input)
```

In the render section, after the existing `refimage_draw(true, ...)` call (the "above" one), add:

```c
            refimage_draw_selection_overlay(canvas_x, CANVAS_Y,
                                            app.canvas.view_x, app.canvas.view_y,
                                            app.canvas.zoom);
```

- [ ] **Step 5: Build**

Run: `make`

Expected: builds. The `refimage_consumed` else branch needs to slot into the existing if/else-if chain inside the `UI_NONE` block. If the chain is linear (space → sizing → zooming → eyedropper → zoom_rect → else-drawing), insert `refimage_consumed` as an early branch after those other modes OR before the `else` for drawing. Recommendation: place refimage_consumed check immediately before the `else { ...drawing... }` branch (so modifier-keys still win).

- [ ] **Step 6: Manual verification**

Run: `make run`

Do:
- Drop an image.
- Click on the image → selection outline appears around it.
- Click on empty canvas → outline disappears (deselect). No stroke starts on that click.
- Press Escape → outline disappears if something was selected.
- Drop a second image on top of the first. Click somewhere inside the overlap — topmost (second) image gets selected.

Expected: all above behaviours work. Brush drawing still works when nothing is selected.

- [ ] **Step 7: Commit**

```bash
git add src/refimage.h src/main.c
git commit -m "refimage: click to select, escape/empty-click to deselect"
```

### Task 8: Move selected image by dragging the body

**Files:**
- Modify: `src/refimage.h`

- [ ] **Step 1: Add drag state and logic**

In REFIMAGE_IMPLEMENTATION, extend the interaction state. Find the `RefImageList` struct and add fields:

```c
typedef struct {
    RefImage *items;
    int count;
    int capacity;
    int selected;
    enum { REF_IDLE, REF_MOVING, REF_SCALING, REF_ROTATING } mode;
    float grab_dx, grab_dy;        // mouse offset from image center at grab time (doc coords)
    int   dirty_after_release;     // 1 = trigger autosave on mouse release
} RefImageList;
```

Update `refimage_init` to zero-initialize the new fields:

```c
void refimage_init(void) {
    g_refs.items = NULL;
    g_refs.count = 0;
    g_refs.capacity = 0;
    g_refs.selected = -1;
    g_refs.mode = REF_IDLE;
    g_refs.grab_dx = 0;
    g_refs.grab_dy = 0;
    g_refs.dirty_after_release = 0;
}
```

- [ ] **Step 2: Extend refimage_update with drag handling**

Replace the `refimage_update` function with this expanded version:

```c
bool refimage_update(int canvas_x, int canvas_y,
                     float view_x, float view_y, float zoom,
                     int panel_w, int panel_h) {
    (void)panel_w; (void)panel_h;
    Vector2 m = GetMousePosition();
    float dx, dy;
    screen_to_doc(m.x, m.y, canvas_x, canvas_y, view_x, view_y, zoom, &dx, &dy);

    // Escape deselects
    if (g_refs.selected != -1 && IsKeyPressed(KEY_ESCAPE) && g_refs.mode == REF_IDLE) {
        g_refs.selected = -1;
        return true;
    }

    // Active drag
    if (g_refs.mode == REF_MOVING && g_refs.selected >= 0) {
        RefImage *r = &g_refs.items[g_refs.selected];
        r->x = dx - g_refs.grab_dx;
        r->y = dy - g_refs.grab_dy;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            g_refs.mode = REF_IDLE;
            g_refs.dirty_after_release = 1;
        }
        return true;
    }

    // Mouse press: hit-test and start drag or select
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        int hit = -1;
        for (int i = g_refs.count - 1; i >= 0; i--) {
            if (point_in_image(&g_refs.items[i], dx, dy)) { hit = i; break; }
        }
        if (hit != -1) {
            g_refs.selected = hit;
            g_refs.mode = REF_MOVING;
            RefImage *r = &g_refs.items[hit];
            g_refs.grab_dx = dx - r->x;
            g_refs.grab_dy = dy - r->y;
            return true;
        }
        if (g_refs.selected != -1) {
            g_refs.selected = -1;
            return true;
        }
    }

    return false;
}
```

- [ ] **Step 3: Add dirty-flag accessor for autosave**

In public API:

```c
// Returns 1 and clears the flag if the user just released a manipulation.
int  refimage_consume_dirty(void);
```

Impl:

```c
int refimage_consume_dirty(void) {
    if (g_refs.dirty_after_release) {
        g_refs.dirty_after_release = 0;
        return 1;
    }
    return 0;
}
```

- [ ] **Step 4: Hook autosave in main.c**

In `src/main.c`, find the autosave block (around line 90):

```c
        {
            static bool prev_drawing = false;
            if (prev_drawing && !app.canvas.is_drawing && app.canvas.dirty && app.db)
                db_autosave(...);
            prev_drawing = app.canvas.is_drawing;
        }
```

Immediately after, add:

```c
        if (refimage_consume_dirty() && app.db) {
            app.canvas.dirty = true;
            db_autosave(app.db, &autosave_id,
                        app.canvas.layers, app.canvas.layer_count,
                        app.canvas.width, app.canvas.height);
        }
```

(Note: the autosave currently doesn't save ref images — we'll add that in Phase 7. For now, just marking the canvas dirty is enough.)

- [ ] **Step 5: Build and run**

Run: `make && ./claude-paint`

Do:
- Drop an image, click to select, drag it around.
- Release mouse → image stays at new position.
- Click elsewhere → deselects. Click image again, drag — position update works.
- While dragging, hold — image follows the cursor precisely (grab offset preserved).

Expected: smooth drag. No snapping on pick-up. No stroke accidentally starts.

- [ ] **Step 6: Commit**

```bash
git add src/refimage.h src/main.c
git commit -m "refimage: drag selected image to move"
```

---

## Phase 6: Resize and rotate

### Task 9: Corner handles for uniform scale

**Files:**
- Modify: `src/refimage.h`

- [ ] **Step 1: Add handle geometry helpers**

Inside REFIMAGE_IMPLEMENTATION, add (above `refimage_update`):

```c
#define HANDLE_SIZE 10.0f   // screen-space px (half-side)
#define HANDLE_ROT_DIST 24.0f

// Fills 4 corner positions in screen space.
// Order: TL, TR, BR, BL.
static void corners_screen(const RefImage *r,
                           int canvas_x, int canvas_y,
                           float view_x, float view_y, float zoom,
                           Vector2 out[4]) {
    float hw = r->src_w * r->scale * 0.5f;
    float hh = r->src_h * r->scale * 0.5f;
    float c = cosf(r->rotation);
    float s = sinf(r->rotation);
    Vector2 local[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    for (int i = 0; i < 4; i++) {
        float dx = r->x + local[i].x * c - local[i].y * s;
        float dy = r->y + local[i].x * s + local[i].y * c;
        out[i].x = canvas_x + view_x + dx * zoom;
        out[i].y = canvas_y + view_y + dy * zoom;
    }
}

// Returns 0..3 if mouse m is over a corner handle, -1 otherwise.
static int hit_corner(const Vector2 corners[4], Vector2 m) {
    for (int i = 0; i < 4; i++) {
        if (fabsf(m.x - corners[i].x) <= HANDLE_SIZE &&
            fabsf(m.y - corners[i].y) <= HANDLE_SIZE)
            return i;
    }
    return -1;
}
```

Add state to track which corner is grabbed:

```c
// inside RefImageList struct:
//   int   scale_corner;          // 0..3 during REF_SCALING
//   float scale_start_dist;      // doc-space distance from center to corner at grab time
//   float scale_start_scale;     // r->scale at grab time
```

Add these fields to the struct. Init them to 0 in `refimage_init`.

- [ ] **Step 2: Update refimage_update to include corner pick**

Replace `refimage_update` with:

```c
bool refimage_update(int canvas_x, int canvas_y,
                     float view_x, float view_y, float zoom,
                     int panel_w, int panel_h) {
    (void)panel_w; (void)panel_h;
    Vector2 m = GetMousePosition();
    float dx, dy;
    screen_to_doc(m.x, m.y, canvas_x, canvas_y, view_x, view_y, zoom, &dx, &dy);

    if (g_refs.selected != -1 && IsKeyPressed(KEY_ESCAPE) && g_refs.mode == REF_IDLE) {
        g_refs.selected = -1;
        return true;
    }

    // Active drag (move)
    if (g_refs.mode == REF_MOVING && g_refs.selected >= 0) {
        RefImage *r = &g_refs.items[g_refs.selected];
        r->x = dx - g_refs.grab_dx;
        r->y = dy - g_refs.grab_dy;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            g_refs.mode = REF_IDLE;
            g_refs.dirty_after_release = 1;
        }
        return true;
    }

    // Active drag (scale)
    if (g_refs.mode == REF_SCALING && g_refs.selected >= 0) {
        RefImage *r = &g_refs.items[g_refs.selected];
        float ddx = dx - r->x;
        float ddy = dy - r->y;
        float dist = sqrtf(ddx * ddx + ddy * ddy);
        float ratio = dist / g_refs.scale_start_dist;
        r->scale = g_refs.scale_start_scale * ratio;
        if (r->scale < 0.05f) r->scale = 0.05f;
        if (r->scale > 20.0f) r->scale = 20.0f;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            g_refs.mode = REF_IDLE;
            g_refs.dirty_after_release = 1;
        }
        return true;
    }

    // Mouse press: check handles on selected, then body hit-test
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_refs.selected >= 0) {
            RefImage *r = &g_refs.items[g_refs.selected];
            Vector2 cor[4];
            corners_screen(r, canvas_x, canvas_y, view_x, view_y, zoom, cor);
            int c = hit_corner(cor, m);
            if (c >= 0) {
                g_refs.mode = REF_SCALING;
                g_refs.scale_corner = c;
                float ddx = dx - r->x;
                float ddy = dy - r->y;
                g_refs.scale_start_dist = sqrtf(ddx * ddx + ddy * ddy);
                if (g_refs.scale_start_dist < 1.0f) g_refs.scale_start_dist = 1.0f;
                g_refs.scale_start_scale = r->scale;
                return true;
            }
        }

        int hit = -1;
        for (int i = g_refs.count - 1; i >= 0; i--) {
            if (point_in_image(&g_refs.items[i], dx, dy)) { hit = i; break; }
        }
        if (hit != -1) {
            g_refs.selected = hit;
            g_refs.mode = REF_MOVING;
            RefImage *r = &g_refs.items[hit];
            g_refs.grab_dx = dx - r->x;
            g_refs.grab_dy = dy - r->y;
            return true;
        }
        if (g_refs.selected != -1) {
            g_refs.selected = -1;
            return true;
        }
    }

    return false;
}
```

- [ ] **Step 3: Draw corner handles in the selection overlay**

Replace `refimage_draw_selection_overlay` with:

```c
void refimage_draw_selection_overlay(int canvas_x, int canvas_y,
                                     float view_x, float view_y, float zoom) {
    if (g_refs.selected < 0 || g_refs.selected >= g_refs.count) return;
    RefImage *r = &g_refs.items[g_refs.selected];

    Vector2 cor[4];
    corners_screen(r, canvas_x, canvas_y, view_x, view_y, zoom, cor);

    // Rectangle outline
    for (int i = 0; i < 4; i++)
        DrawLineEx(cor[i], cor[(i + 1) % 4], 1.5f, (Color){0, 0, 0, 180});

    // Corner handles
    for (int i = 0; i < 4; i++) {
        Rectangle h = {cor[i].x - HANDLE_SIZE, cor[i].y - HANDLE_SIZE,
                       HANDLE_SIZE * 2.0f, HANDLE_SIZE * 2.0f};
        DrawRectangleRec(h, WHITE);
        DrawRectangleLinesEx(h, 1.0f, (Color){0, 0, 0, 200});
    }
}
```

- [ ] **Step 4: Build and verify**

Run: `make && ./claude-paint`

Do:
- Drop image, click to select.
- Drag any corner → image scales uniformly. Pulling outward grows, pulling inward shrinks.
- Release → size holds.
- Drag body (away from corner) → move still works.

Expected: smooth scaling. No jump on first drag pixel.

- [ ] **Step 5: Commit**

```bash
git add src/refimage.h
git commit -m "refimage: corner handles for uniform scale"
```

### Task 10: Rotation handle

**Files:**
- Modify: `src/refimage.h`

- [ ] **Step 1: Add rotation handle state**

In the `RefImageList` struct:

```c
    float rot_start_angle;    // mouse angle at grab time
    float rot_start_rot;      // r->rotation at grab time
```

Init to 0 in `refimage_init`.

- [ ] **Step 2: Add helper + hit-test for rotation handle**

Inside REFIMAGE_IMPLEMENTATION:

```c
// Rotation handle position in screen space: centered above the top edge midpoint,
// offset along the image's up-axis (rotated).
static Vector2 rotation_handle_screen(const RefImage *r,
                                      int canvas_x, int canvas_y,
                                      float view_x, float view_y, float zoom) {
    float hh = r->src_h * r->scale * 0.5f;
    // Image-local offset: straight up from top edge mid
    float local_y = -(hh + HANDLE_ROT_DIST / zoom);
    float c = cosf(r->rotation);
    float s = sinf(r->rotation);
    float dx = r->x + 0 * c - local_y * s;
    float dy = r->y + 0 * s + local_y * c;
    return (Vector2){canvas_x + view_x + dx * zoom,
                     canvas_y + view_y + dy * zoom};
}

static bool hit_rotation_handle(const RefImage *r,
                                int canvas_x, int canvas_y,
                                float view_x, float view_y, float zoom,
                                Vector2 m) {
    Vector2 h = rotation_handle_screen(r, canvas_x, canvas_y, view_x, view_y, zoom);
    float dx = m.x - h.x, dy = m.y - h.y;
    return dx * dx + dy * dy <= 12.0f * 12.0f;
}
```

- [ ] **Step 3: Handle rotation in refimage_update**

Replace the "Mouse press: check handles" block in `refimage_update` to include rotation first, then corners:

```c
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (g_refs.selected >= 0) {
            RefImage *r = &g_refs.items[g_refs.selected];

            if (hit_rotation_handle(r, canvas_x, canvas_y,
                                    view_x, view_y, zoom, m)) {
                g_refs.mode = REF_ROTATING;
                g_refs.rot_start_angle = atan2f(dy - r->y, dx - r->x);
                g_refs.rot_start_rot   = r->rotation;
                return true;
            }

            Vector2 cor[4];
            corners_screen(r, canvas_x, canvas_y, view_x, view_y, zoom, cor);
            int c = hit_corner(cor, m);
            if (c >= 0) {
                g_refs.mode = REF_SCALING;
                g_refs.scale_corner = c;
                float ddx = dx - r->x;
                float ddy = dy - r->y;
                g_refs.scale_start_dist = sqrtf(ddx * ddx + ddy * ddy);
                if (g_refs.scale_start_dist < 1.0f) g_refs.scale_start_dist = 1.0f;
                g_refs.scale_start_scale = r->scale;
                return true;
            }
        }
        // ... rest unchanged
```

Add a rotation-drag handler alongside the SCALING drag handler:

```c
    if (g_refs.mode == REF_ROTATING && g_refs.selected >= 0) {
        RefImage *r = &g_refs.items[g_refs.selected];
        float ang = atan2f(dy - r->y, dx - r->x);
        r->rotation = g_refs.rot_start_rot + (ang - g_refs.rot_start_angle);
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            g_refs.mode = REF_IDLE;
            g_refs.dirty_after_release = 1;
        }
        return true;
    }
```

Place this block next to the MOVING and SCALING blocks near the top of the function.

- [ ] **Step 4: Draw the rotation handle in the overlay**

At the end of `refimage_draw_selection_overlay`, add:

```c
    Vector2 rh = rotation_handle_screen(r, canvas_x, canvas_y, view_x, view_y, zoom);
    DrawCircleV(rh, 6.0f, WHITE);
    DrawCircleLinesV(rh, 6.0f, (Color){0, 0, 0, 200});
    // Line from top-edge-midpoint to handle
    Vector2 top_mid = {(cor[0].x + cor[1].x) * 0.5f, (cor[0].y + cor[1].y) * 0.5f};
    DrawLineEx(top_mid, rh, 1.0f, (Color){0, 0, 0, 180});
```

- [ ] **Step 5: Build and verify**

Run: `make && ./claude-paint`

Do:
- Drop image, click to select.
- Drag the small circle above the top edge → image rotates around its center.
- Release → rotation holds.
- Rotate ~45°, then drag a corner → scaling still works on the rotated image.
- Rotate 180° (image appears upside down). Move and scale still work.

Expected: smooth rotation. No jump on grab. Corner scaling uses diagonal distance from center, so it works correctly even when rotated.

- [ ] **Step 6: Commit**

```bash
git add src/refimage.h
git commit -m "refimage: rotation handle and drag-to-rotate"
```

---

## Phase 7: Floating panel

### Task 11: Opacity slider, above/below toggle, delete button

**Files:**
- Modify: `src/refimage.h`

- [ ] **Step 1: Add panel state**

In `RefImageList`:

```c
    bool slider_dragging;   // mouse held on opacity slider
```

Init false.

- [ ] **Step 2: Panel drawing and hit-testing helpers**

Inside REFIMAGE_IMPLEMENTATION:

```c
#define PANEL_W 220.0f
#define PANEL_H 80.0f

// AABB of the selected image in screen space; returns min/max corners.
static void selection_aabb(const RefImage *r,
                           int canvas_x, int canvas_y,
                           float view_x, float view_y, float zoom,
                           float *min_x, float *min_y,
                           float *max_x, float *max_y) {
    Vector2 cor[4];
    corners_screen(r, canvas_x, canvas_y, view_x, view_y, zoom, cor);
    *min_x = cor[0].x; *min_y = cor[0].y;
    *max_x = cor[0].x; *max_y = cor[0].y;
    for (int i = 1; i < 4; i++) {
        if (cor[i].x < *min_x) *min_x = cor[i].x;
        if (cor[i].y < *min_y) *min_y = cor[i].y;
        if (cor[i].x > *max_x) *max_x = cor[i].x;
        if (cor[i].y > *max_y) *max_y = cor[i].y;
    }
}

static Rectangle panel_rect(const RefImage *r,
                            int canvas_x, int canvas_y,
                            float view_x, float view_y, float zoom,
                            int panel_w, int panel_h) {
    float mnx, mny, mxx, mxy;
    selection_aabb(r, canvas_x, canvas_y, view_x, view_y, zoom,
                   &mnx, &mny, &mxx, &mxy);
    float px = (mnx + mxx) * 0.5f - PANEL_W * 0.5f;
    float py = mxy + 12.0f;
    if (py + PANEL_H > canvas_y + panel_h) {
        py = mny - 12.0f - PANEL_H;
    }
    if (px < canvas_x + 4) px = canvas_x + 4;
    if (px + PANEL_W > canvas_x + panel_w - 4) px = canvas_x + panel_w - PANEL_W - 4;
    if (py < canvas_y + 4) py = canvas_y + 4;
    return (Rectangle){px, py, PANEL_W, PANEL_H};
}
```

- [ ] **Step 3: Add panel input handling at top of refimage_update**

Immediately after `screen_to_doc(...)` and the Escape check, insert:

```c
    // Panel input (only when something is selected)
    if (g_refs.selected >= 0) {
        RefImage *r = &g_refs.items[g_refs.selected];
        Rectangle pr = panel_rect(r, canvas_x, canvas_y, view_x, view_y, zoom,
                                  panel_w, panel_h);

        // Slider geometry: inside the panel, top row
        Rectangle slider = {pr.x + 12, pr.y + 16, pr.width - 24, 10};
        // Toggle geometry: middle row
        Rectangle toggle_above = {pr.x + 12, pr.y + 36, (pr.width - 36) * 0.5f, 20};
        Rectangle toggle_below = {toggle_above.x + toggle_above.width + 4,
                                  pr.y + 36, toggle_above.width, 20};
        // Delete button: right edge
        Rectangle del_btn = {pr.x + pr.width - 30, pr.y + pr.height - 26, 22, 20};

        Vector2 mpos = m;

        // Continue dragging slider
        if (g_refs.slider_dragging) {
            float t = (mpos.x - slider.x) / slider.width;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            r->opacity = t;
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                g_refs.slider_dragging = false;
                g_refs.dirty_after_release = 1;
            }
            return true;
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(mpos, pr)) {
            if (CheckCollisionPointRec(mpos, slider)) {
                g_refs.slider_dragging = true;
                float t = (mpos.x - slider.x) / slider.width;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                r->opacity = t;
                return true;
            }
            if (CheckCollisionPointRec(mpos, toggle_above)) {
                r->above_strokes = true;
                g_refs.dirty_after_release = 1;
                return true;
            }
            if (CheckCollisionPointRec(mpos, toggle_below)) {
                r->above_strokes = false;
                g_refs.dirty_after_release = 1;
                return true;
            }
            if (CheckCollisionPointRec(mpos, del_btn)) {
                // Remove selected image
                free_item(r);
                memmove(&g_refs.items[g_refs.selected],
                        &g_refs.items[g_refs.selected + 1],
                        (g_refs.count - g_refs.selected - 1) * sizeof(RefImage));
                g_refs.count--;
                g_refs.selected = -1;
                g_refs.dirty_after_release = 1;
                return true;
            }
            // Click inside panel but not on a control: still consume
            return true;
        }
    }
```

- [ ] **Step 4: Draw the panel after selection overlay**

Add to `refimage_draw_selection_overlay`, right after the rotation handle draw, but we need panel_w/h — change the signature of this function to take them. Easier: create a new function `refimage_draw_panel`.

In public API:

```c
void refimage_draw_panel(int canvas_x, int canvas_y,
                        float view_x, float view_y, float zoom,
                        int panel_w, int panel_h);
```

Impl:

```c
void refimage_draw_panel(int canvas_x, int canvas_y,
                        float view_x, float view_y, float zoom,
                        int panel_w, int panel_h) {
    if (g_refs.selected < 0 || g_refs.selected >= g_refs.count) return;
    RefImage *r = &g_refs.items[g_refs.selected];
    Rectangle pr = panel_rect(r, canvas_x, canvas_y, view_x, view_y, zoom,
                              panel_w, panel_h);

    DrawRectangleRec(pr, (Color){30, 30, 30, 230});
    DrawRectangleLinesEx(pr, 1.0f, (Color){80, 80, 80, 255});

    // Opacity slider
    Rectangle slider = {pr.x + 12, pr.y + 16, pr.width - 24, 10};
    DrawRectangleRec(slider, (Color){50, 50, 50, 255});
    Rectangle fill = slider;
    fill.width *= r->opacity;
    DrawRectangleRec(fill, (Color){180, 180, 200, 255});
    DrawRectangleLinesEx(slider, 1.0f, (Color){90, 90, 90, 255});

    // Toggle pill
    Rectangle ta = {pr.x + 12, pr.y + 36, (pr.width - 36) * 0.5f, 20};
    Rectangle tb = {ta.x + ta.width + 4, pr.y + 36, ta.width, 20};
    Color on = {90, 110, 150, 255};
    Color off = {50, 50, 50, 255};
    DrawRectangleRec(ta, r->above_strokes ? on : off);
    DrawRectangleRec(tb, r->above_strokes ? off : on);
    DrawRectangleLinesEx(ta, 1.0f, (Color){80, 80, 80, 255});
    DrawRectangleLinesEx(tb, 1.0f, (Color){80, 80, 80, 255});

    // Delete button
    Rectangle del = {pr.x + pr.width - 30, pr.y + pr.height - 26, 22, 20};
    DrawRectangleRec(del, (Color){120, 40, 40, 255});
    DrawRectangleLinesEx(del, 1.0f, (Color){200, 80, 80, 255});
}
```

- [ ] **Step 5: Call the panel draw in main.c**

In `src/main.c`, after `refimage_draw_selection_overlay(...)`:

```c
            refimage_draw_panel(canvas_x, CANVAS_Y,
                                app.canvas.view_x, app.canvas.view_y,
                                app.canvas.zoom,
                                app.canvas.rt.texture.width,
                                app.canvas.rt.texture.height);
```

- [ ] **Step 6: Build and verify**

Run: `make && ./claude-paint`

Do:
- Drop image, click to select.
- Verify: panel appears under (or above) the image.
- Drag slider → image opacity changes live.
- Click the "Below" side of the pill → image now sits behind any drawn strokes (draw some strokes to check).
- Click the red delete button → image removed, panel disappears.
- Drop two images, select one, click the other → panel repositions to the newly selected image.

Expected: all interactions work. Panel doesn't go offscreen (clamps to canvas area).

- [ ] **Step 7: Commit**

```bash
git add src/refimage.h src/main.c
git commit -m "refimage: floating panel with opacity, above/below, delete"
```

### Task 12: Delete and Escape keyboard shortcuts

**Files:**
- Modify: `src/refimage.h`

- [ ] **Step 1: Handle Delete/Backspace in refimage_update**

Near the Escape check (at top of `refimage_update`), add:

```c
    if (g_refs.selected != -1 && g_refs.mode == REF_IDLE &&
        (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE))) {
        RefImage *r = &g_refs.items[g_refs.selected];
        free_item(r);
        memmove(&g_refs.items[g_refs.selected],
                &g_refs.items[g_refs.selected + 1],
                (g_refs.count - g_refs.selected - 1) * sizeof(RefImage));
        g_refs.count--;
        g_refs.selected = -1;
        g_refs.dirty_after_release = 1;
        return true;
    }
```

- [ ] **Step 2: Build and verify**

Run: `make && ./claude-paint`

Do:
- Drop image, select it, press Delete → image removed.
- Drop image, select it, press Backspace → image removed.
- Escape with selection → deselects.

Expected: works. No crash if nothing is selected.

- [ ] **Step 3: Commit**

```bash
git add src/refimage.h
git commit -m "refimage: Delete/Backspace keys remove selected image"
```

---

## Phase 8: Persistence

### Task 13: Add ref_images table and bump schema version

**Files:**
- Modify: `src/db.c`

- [ ] **Step 1: Extend SCHEMA_SQL**

In `src/db.c`, replace the `SCHEMA_SQL` string (ending at the `;;");";` of strokes) with:

```c
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

    "CREATE TABLE IF NOT EXISTS layers ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  painting_id INTEGER NOT NULL REFERENCES paintings(id) ON DELETE CASCADE,"
    "  layer_idx   INTEGER NOT NULL,"
    "  name        TEXT    NOT NULL,"
    "  visible     INTEGER NOT NULL DEFAULT 1"
    ");"

    "CREATE TABLE IF NOT EXISTS strokes ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  painting_id INTEGER NOT NULL REFERENCES paintings(id) ON DELETE CASCADE,"
    "  layer_id    INTEGER NOT NULL REFERENCES layers(id) ON DELETE CASCADE,"
    "  stroke_idx  INTEGER NOT NULL,"
    "  color_r     INTEGER NOT NULL,"
    "  color_g     INTEGER NOT NULL,"
    "  color_b     INTEGER NOT NULL,"
    "  color_a     INTEGER NOT NULL,"
    "  radius      INTEGER NOT NULL,"
    "  tool        INTEGER NOT NULL,"
    "  points      BLOB    NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS ref_images ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  painting_id   INTEGER NOT NULL REFERENCES paintings(id) ON DELETE CASCADE,"
    "  image_idx     INTEGER NOT NULL,"
    "  x             REAL    NOT NULL,"
    "  y             REAL    NOT NULL,"
    "  scale         REAL    NOT NULL,"
    "  rotation      REAL    NOT NULL,"
    "  opacity       REAL    NOT NULL,"
    "  above_strokes INTEGER NOT NULL,"
    "  png_blob      BLOB    NOT NULL"
    ");"

    "CREATE INDEX IF NOT EXISTS idx_ref_images_painting "
    "ON ref_images(painting_id, image_idx);";
```

- [ ] **Step 2: Bump schema version and drop table in migration**

In `src/db.c`, change:

```c
#define SCHEMA_VERSION 3
```

to:

```c
#define SCHEMA_VERSION 4
```

And in `migrate`, add a line to drop the new table when rebuilding:

```c
    sqlite3_exec(db, "DROP TABLE IF EXISTS ref_images;", NULL, NULL, NULL);
    sqlite3_exec(db, "DROP TABLE IF EXISTS strokes;",    NULL, NULL, NULL);
    sqlite3_exec(db, "DROP TABLE IF EXISTS layers;",     NULL, NULL, NULL);
    sqlite3_exec(db, "DROP TABLE IF EXISTS paintings;",  NULL, NULL, NULL);
```

- [ ] **Step 3: Build**

Run: `make`

Expected: builds. On next run, the DB at `~/Library/Application Support/claude-paint/paintings.db` will be rebuilt (previous paintings lost — OK for a single-user dev app).

- [ ] **Step 4: Commit**

```bash
git add src/db.c
git commit -m "db: add ref_images table, bump schema version to 4"
```

### Task 14: Implement db_save_ref_images / db_load_ref_images / db_free_ref_images

**Files:**
- Modify: `src/db.h`
- Modify: `src/db.c`

- [ ] **Step 1: Extend db.h**

At the top of `src/db.h`, after other includes, add:

```c
typedef struct RefImage RefImage;   // forward-declare (defined in refimage.h)
```

This is compatible with the named struct `typedef struct RefImage { ... } RefImage;` from Task 3.

Add to `src/db.h`:

```c
// Wipe & rewrite ref_images for this painting. Returns 0 on success.
int  db_save_ref_images(sqlite3 *db, int painting_id,
                        const RefImage *images, int count);

// Loads all ref images for painting_id into a freshly allocated array.
// Each entry's texture is uploaded and png_bytes is malloc'd.
// Returns true on success (even if 0 rows). Caller owns *out; free with
// db_free_ref_images.
bool db_load_ref_images(sqlite3 *db, int painting_id,
                        RefImage **out, int *out_count);

// Free a loaded array: unloads textures, frees png bytes, frees array.
void db_free_ref_images(RefImage *images, int count);
```

- [ ] **Step 2: Include refimage.h from db.c**

At the top of `src/db.c`, add:

```c
#include "refimage.h"
```

This brings in `RefImage`. No need for `REFIMAGE_IMPLEMENTATION` here — db.c only uses the type.

- [ ] **Step 3: Implement the three functions in db.c**

At the bottom of `src/db.c`, add:

```c
// ── Reference images ─────────────────────────────────────────────────────────

int db_save_ref_images(sqlite3 *db, int painting_id,
                       const RefImage *images, int count) {
    sqlite3_stmt *del_stmt, *ins_stmt;
    const char *del = "DELETE FROM ref_images WHERE painting_id = ?;";
    const char *ins =
        "INSERT INTO ref_images "
        "(painting_id, image_idx, x, y, scale, rotation, opacity, above_strokes, png_blob) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";

    if (sqlite3_prepare_v2(db, del, -1, &del_stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(del_stmt, 1, painting_id);
    if (sqlite3_step(del_stmt) != SQLITE_DONE) {
        sqlite3_finalize(del_stmt);
        return -1;
    }
    sqlite3_finalize(del_stmt);

    if (sqlite3_prepare_v2(db, ins, -1, &ins_stmt, NULL) != SQLITE_OK) return -1;

    for (int i = 0; i < count; i++) {
        const RefImage *r = &images[i];
        sqlite3_reset(ins_stmt);
        sqlite3_bind_int   (ins_stmt, 1, painting_id);
        sqlite3_bind_int   (ins_stmt, 2, i);
        sqlite3_bind_double(ins_stmt, 3, r->x);
        sqlite3_bind_double(ins_stmt, 4, r->y);
        sqlite3_bind_double(ins_stmt, 5, r->scale);
        sqlite3_bind_double(ins_stmt, 6, r->rotation);
        sqlite3_bind_double(ins_stmt, 7, r->opacity);
        sqlite3_bind_int   (ins_stmt, 8, r->above_strokes ? 1 : 0);
        sqlite3_bind_blob  (ins_stmt, 9, r->png_bytes, r->png_len, SQLITE_TRANSIENT);

        if (sqlite3_step(ins_stmt) != SQLITE_DONE) {
            fprintf(stderr, "db_save_ref_images %d: %s\n", i, sqlite3_errmsg(db));
            sqlite3_finalize(ins_stmt);
            return -1;
        }
    }
    sqlite3_finalize(ins_stmt);
    return 0;
}

bool db_load_ref_images(sqlite3 *db, int painting_id,
                        RefImage **out, int *out_count) {
    *out = NULL;
    *out_count = 0;

    sqlite3_stmt *stmt;
    const char *sel =
        "SELECT id, x, y, scale, rotation, opacity, above_strokes, png_blob "
        "FROM ref_images WHERE painting_id = ? ORDER BY image_idx ASC;";
    if (sqlite3_prepare_v2(db, sel, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, painting_id);

    int cap = 0, cnt = 0;
    RefImage *arr = NULL;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (cnt == cap) {
            cap = cap ? cap * 2 : 4;
            arr = realloc(arr, cap * sizeof(RefImage));
        }
        RefImage *r = &arr[cnt];
        r->db_id         = sqlite3_column_int   (stmt, 0);
        r->x             = (float)sqlite3_column_double(stmt, 1);
        r->y             = (float)sqlite3_column_double(stmt, 2);
        r->scale         = (float)sqlite3_column_double(stmt, 3);
        r->rotation      = (float)sqlite3_column_double(stmt, 4);
        r->opacity       = (float)sqlite3_column_double(stmt, 5);
        r->above_strokes = sqlite3_column_int   (stmt, 6) != 0;

        const void *blob = sqlite3_column_blob(stmt, 7);
        int blob_len     = sqlite3_column_bytes(stmt, 7);
        r->png_bytes = malloc(blob_len);
        memcpy(r->png_bytes, blob, blob_len);
        r->png_len   = blob_len;

        Image img = LoadImageFromMemory(".png", r->png_bytes, blob_len);
        if (!img.data) {
            // Try other formats
            img = LoadImageFromMemory(".jpg", r->png_bytes, blob_len);
        }
        if (!img.data) {
            fprintf(stderr, "db_load_ref_images: failed to decode image %d\n", cnt);
            free(r->png_bytes);
            continue;
        }
        r->src_w = img.width;
        r->src_h = img.height;
        r->tex = LoadTextureFromImage(img);
        UnloadImage(img);

        cnt++;
    }
    sqlite3_finalize(stmt);

    *out = arr;
    *out_count = cnt;
    return true;
}

void db_free_ref_images(RefImage *images, int count) {
    if (!images) return;
    for (int i = 0; i < count; i++) {
        if (images[i].tex.id) UnloadTexture(images[i].tex);
        free(images[i].png_bytes);
    }
    free(images);
}
```

- [ ] **Step 4: Build**

Run: `make`

Expected: builds with no errors. If `LoadImageFromMemory` signature differs, check raylib docs — should be `LoadImageFromMemory(const char *fileType, const unsigned char *fileData, int dataSize)`.

- [ ] **Step 5: Commit**

```bash
git add src/db.h src/db.c src/refimage.h
git commit -m "db: save/load/free ref_images with png blob"
```

### Task 15: Hook ref image save/load into painting save/load flow

**Files:**
- Modify: `src/db.c`
- Modify: `src/main.c`
- Modify: `src/ui.c` (load path)

- [ ] **Step 1: Modify db_save_painting to also save ref images**

In `src/db.c`, find `db_save_painting`. Change its signature to accept ref images:

```c
int db_save_painting(sqlite3 *db, const char *name,
                     const Layer *layers, int layer_count,
                     const RefImage *refs, int ref_count,
                     int width, int height);
```

Update `src/db.h` with the new signature.

At the end of `db_save_painting` (just before the final `return painting_id;` but after `COMMIT;`), the commit happens while stmt is still valid. Simpler: add the ref image save INSIDE the transaction, before COMMIT.

Restructure: after the final `for (int li = 0; li < layer_count; li++)` loop completes, and before `sqlite3_finalize(layer_stmt);`, insert:

```c
    if (ref_count > 0) {
        const char *ins_ref =
            "INSERT INTO ref_images "
            "(painting_id, image_idx, x, y, scale, rotation, opacity, above_strokes, png_blob) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt *ref_stmt;
        if (sqlite3_prepare_v2(db, ins_ref, -1, &ref_stmt, NULL) != SQLITE_OK) {
            goto fail;
        }
        for (int ri = 0; ri < ref_count; ri++) {
            const RefImage *r = &refs[ri];
            sqlite3_reset(ref_stmt);
            sqlite3_bind_int   (ref_stmt, 1, painting_id);
            sqlite3_bind_int   (ref_stmt, 2, ri);
            sqlite3_bind_double(ref_stmt, 3, r->x);
            sqlite3_bind_double(ref_stmt, 4, r->y);
            sqlite3_bind_double(ref_stmt, 5, r->scale);
            sqlite3_bind_double(ref_stmt, 6, r->rotation);
            sqlite3_bind_double(ref_stmt, 7, r->opacity);
            sqlite3_bind_int   (ref_stmt, 8, r->above_strokes ? 1 : 0);
            sqlite3_bind_blob  (ref_stmt, 9, r->png_bytes, r->png_len, SQLITE_TRANSIENT);
            if (sqlite3_step(ref_stmt) != SQLITE_DONE) {
                sqlite3_finalize(ref_stmt);
                goto fail;
            }
        }
        sqlite3_finalize(ref_stmt);
    }
```

- [ ] **Step 2: Update db_autosave signature + call site**

In `src/db.h`:

```c
void db_autosave(sqlite3 *db, int *autosave_id,
                 const Layer *layers, int layer_count,
                 const RefImage *refs, int ref_count,
                 int width, int height);
```

In `src/db.c`, update the body of `db_autosave` to pass through:

```c
void db_autosave(sqlite3 *db, int *autosave_id,
                 const Layer *layers, int layer_count,
                 const RefImage *refs, int ref_count,
                 int width, int height) {
    if (*autosave_id > 0) {
        sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
        char sql[128];
        snprintf(sql, sizeof(sql), "DELETE FROM paintings WHERE id = %d;", *autosave_id);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    *autosave_id = db_save_painting(db, "_autosave", layers, layer_count,
                                    refs, ref_count, width, height);
}
```

- [ ] **Step 3: Update call sites in main.c**

In `src/main.c`, find both `db_autosave` calls (one in the autosave-after-stroke block, one in the ref image dirty hook from Task 8, one in the cleanup block at end of main). Update each to:

```c
            // Build a RefImage array from the refimage module
            int nrefs = refimage_count();
            RefImage *refs = nrefs > 0 ? malloc(nrefs * sizeof(RefImage)) : NULL;
            for (int i = 0; i < nrefs; i++) refs[i] = *refimage_get(i);
            db_autosave(app.db, &autosave_id,
                        app.canvas.layers, app.canvas.layer_count,
                        refs, nrefs,
                        app.canvas.width, app.canvas.height);
            free(refs);
```

Note: we pass shallow copies of `RefImage`. That's fine because `db_save_painting` reads `png_bytes` + `png_len` during the INSERT with `SQLITE_TRANSIENT` (which copies), and we don't free png_bytes in this flow. The shallow copy doesn't own anything.

Also at the final cleanup near `db_autosave(...)` at the end of `main()`, apply the same change.

- [ ] **Step 4: Update `db_save_painting` caller from ui.c**

Grep for `db_save_painting` call sites:

Run: `grep -rn "db_save_painting" src/`

Update each call (likely in `src/ui.c` for the save dialog confirm path) to include refs:

```c
            int nrefs = refimage_count();
            RefImage *refs = nrefs > 0 ? malloc(nrefs * sizeof(RefImage)) : NULL;
            for (int i = 0; i < nrefs; i++) refs[i] = *refimage_get(i);
            db_save_painting(db, name,
                             canvas->layers, canvas->layer_count,
                             refs, nrefs,
                             canvas->width, canvas->height);
            free(refs);
```

In `src/ui.c`, add `#include "refimage.h"` at the top (no REFIMAGE_IMPLEMENTATION — just the header).

- [ ] **Step 5: Update db_load_painting signature to load ref images**

In `src/db.h`:

```c
bool db_load_painting(sqlite3 *db, int id,
                      Layer **out_layers, int *out_layer_count,
                      RefImage **out_refs, int *out_ref_count,
                      int *out_width, int *out_height);
```

In `src/db.c`, extend `db_load_painting` at the end (just before `return true;`) to also load ref images:

```c
    if (out_refs && out_ref_count) {
        if (!db_load_ref_images(db, id, out_refs, out_ref_count)) {
            *out_refs = NULL;
            *out_ref_count = 0;
        }
    }
```

- [ ] **Step 6: Update load-path callers**

Grep for `db_load_painting`:

Run: `grep -rn "db_load_painting" src/`

In each caller (likely `src/ui.c`), extend the call to load refs, then push them into refimage:

```c
            Layer *layers = NULL;
            int lc = 0;
            RefImage *refs = NULL;
            int rc = 0;
            int w = 0, h = 0;
            if (db_load_painting(db, painting_id,
                                 &layers, &lc, &refs, &rc, &w, &h)) {
                canvas_load_layers(canvas, layers, lc);
                refimage_load_from_db(refs, rc);   // takes ownership
                // ... existing post-load setup ...
            }
```

- [ ] **Step 7: Clear refimage on new painting**

In `src/main.c`, in the `if (ev.wants_new) { ... }` block:

```c
            if (ev.wants_new) {
                canvas_clear(&app.canvas);
                refimage_clear();
                autosave_id = 0;
                app.canvas_name[0] = '\0';
                update_title(&app);
            }
```

- [ ] **Step 8: Build**

Run: `make`

Expected: builds with no errors. If there are linker errors for missing `refimage_*` symbols in db.c, check that db.c only references `RefImage` (the type), not any refimage function. The type comes from `#include "refimage.h"` without defining REFIMAGE_IMPLEMENTATION.

- [ ] **Step 9: Manual verification**

Run: `make run`

Do:
- Drop an image onto the canvas, resize/rotate it, set opacity 50%, toggle to "Below".
- Save the painting via toolbar Save button.
- Quit the app.
- Relaunch: `./claude-paint`.
- Open Load dialog, select the painting.
- Expected: image reloads at the exact position, scale, rotation, opacity, and above/below state. Strokes are also preserved.
- New → canvas clears, image disappears.
- Drop image → autosave runs (watch stderr for any errors; no visible message).

- [ ] **Step 10: Commit**

```bash
git add src/db.h src/db.c src/ui.c src/main.c
git commit -m "persist ref images: save on save, load on load, clear on new"
```

---

## Phase 9: Polish

### Task 16: Cursor feedback and edge-case handling

**Files:**
- Modify: `src/refimage.h`

- [ ] **Step 1: Cursor hints when over handles**

In `refimage_update`, near the top (right after getting `m` and computing `dx, dy`), add a cursor hint block that runs every frame when something is selected but we're not dragging:

```c
    if (g_refs.selected >= 0 && g_refs.mode == REF_IDLE) {
        RefImage *r = &g_refs.items[g_refs.selected];
        Vector2 cor[4];
        corners_screen(r, canvas_x, canvas_y, view_x, view_y, zoom, cor);
        if (hit_rotation_handle(r, canvas_x, canvas_y, view_x, view_y, zoom, m)) {
            SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        } else if (hit_corner(cor, m) >= 0) {
            SetMouseCursor(MOUSE_CURSOR_RESIZE_ALL);
        } else if (point_in_image(r, dx, dy)) {
            SetMouseCursor(MOUSE_CURSOR_RESIZE_ALL);
        }
    }
```

Note: the main.c else-branch for the standard drawing flow sets `MOUSE_CURSOR_DEFAULT` each frame. Our refimage hints apply only when `refimage_update` runs first, which it does before that branch. But the "drawing else" branch still runs when nothing is selected. Fine — no conflict.

- [ ] **Step 2: Guard against zero-size scale**

In `refimage_update`'s scale-drag handler, if `g_refs.scale_start_dist` is near-zero (shouldn't happen but paranoia), skip. Already guarded by `if (g_refs.scale_start_dist < 1.0f) g_refs.scale_start_dist = 1.0f;`.

- [ ] **Step 3: Build and verify**

Run: `make && ./claude-paint`

Do:
- Drop image, select, hover cursor over handles → cursor changes.
- Hover over rotation circle → pointer cursor.
- Hover over body → move cursor.

Expected: cursor gives immediate affordance feedback.

- [ ] **Step 4: Commit**

```bash
git add src/refimage.h
git commit -m "refimage: cursor hints over handles and body"
```

---

## Self-Review Checklist

The plan author should re-read the spec and check each requirement is implemented by a task above:

- [x] Drag-and-drop from Finder → Task 4
- [x] Cmd/Ctrl+V paste → Task 5
- [x] Multiple images, click to select → Task 7
- [x] Move by drag → Task 8
- [x] Uniform resize by corner → Task 9
- [x] Rotate by handle → Task 10
- [x] Floating panel with opacity slider → Task 11
- [x] Above/Below strokes toggle per image → Task 11
- [x] Delete via panel button → Task 11
- [x] Delete/Backspace key → Task 12
- [x] Escape to deselect → Task 7
- [x] Persistence per painting → Task 13, 14, 15
- [x] Cascade delete with painting → schema `ON DELETE CASCADE` in Task 13
- [x] Paper + below-refs + strokes + above-refs render order → Task 1, 2, 6
- [x] No unit tests — project has none; plan uses manual verification as the test step, with visual checks explicitly called out each phase

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-17-reference-images.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — I execute tasks in this session using executing-plans, batching with checkpoints for review.

**Which approach?**
