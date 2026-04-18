# Understudy

A keyboard-driven sketching app for macOS. A big canvas, layered ink strokes, reference images you can drop or paste in, and modifier keys that turn the mouse into a brush, a zoom lens, a size scrubber, or a color picker depending on what you're holding.

Built in C with [raylib](https://www.raylib.com/) for rendering and SQLite for storage. One 4096×4096 document, rendered through a paper shader and an ink-compositing shader so strokes feel a little less digital.

## Build

```bash
brew install raylib sqlite
make run
```

macOS only — clipboard paste is AppKit, and the Makefile links `Cocoa`, `IOKit`, `OpenGL`, and `CoreVideo`.

## What's in it

- **Layered vector strokes.** Every stroke is a list of sample points replayed through the ink shader. Undo pops the last stroke. Each layer has its own opacity, visibility, and pan offset.
- **Reference images.** Drop a file onto the window, or `Cmd-V` a screenshot. Reference images live in the same z-stack as stroke layers, so you can slip ink in front of or behind them.
- **Fluid navigation.** Scroll to zoom at the cursor, `Space`-drag to pan, `1`–`5` for preset zooms, `Z`-drag to zoom to a rectangle. A minimap fades in whenever you move the view.
- **Keyboard tools.** Hold `D` and drag to scrub brush size. Hold `F` to open a radial color picker — release over a swatch to pick it. Hold `E` for an eyedropper. Hold `Shift` for a straight line. Press `/` for the help overlay.
- **Autosave.** Every completed stroke writes to SQLite at `~/Library/Application Support/Understudy/paintings.db`. Save with a name when you want to keep it; load, rename, delete, export as PNG, crop, or resize from the toolbar.

## Files

```
src/
  main.c              game loop + all keyboard routing
  canvas.c/.h         layers, strokes, render targets, shader compositing
  refimage.h          single-header reference-image system
  toolbar.c/.h        left panel
  ui.c/.h             modal overlays (save/load/export/crop/resize/help/layer settings)
  db.c/.h             SQLite schema and I/O
  tools.c/.h          active tool + brush state
  clipboard_mac.m     NSPasteboard → PNG bytes
  paper.fs, ink.fs    GLSL fragment shaders
```

See [CLAUDE.md](CLAUDE.md) for architecture notes.

## Credits

Written collaboratively with [Claude Code](https://claude.com/claude-code), Anthropic's terminal coding agent. Most of the code in this repo was produced by driving Claude through the design: sketching a feature, having Claude propose an approach, trying it, and iterating. The shaders, the ref-image layering, the keyboard-heavy interaction model, and the storage schema all came out of that loop. Any rough edges are mine.
