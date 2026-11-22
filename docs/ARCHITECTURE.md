# Architecture

This describes the target architecture. Sections marked *(M2+)* are designed now so that M1 does
not paint us into a corner, but they are implemented later.

## Modules and dependencies

```
            main.c
              │
     ┌────────┼─────────┐
   game/     ui/      render/
     │        │         │
     └────────┴────┬────┘
                  sim/  ──►  core/  ──►  SDL3 (threads, atomics, timer only)
```

| Module | Responsibility | May depend on |
|---|---|---|
| `core/` | Basic types, `PR_ASSERT`, logging, RNG, thread pool, profiler timers, aligned allocation | SDL3 (non-video parts) |
| `sim/` | World and chunk storage, element table, movement pass, heat pass, edit application | `core/` |
| `render/` | Converting cells to RGBA, texture upload, overlays, drawing the world to the screen | `sim/`, `core/`, SDL3 render |
| `ui/` | Immediate-mode widgets (button, label, panel), toolbar, HUD | `core/`, SDL3 render |
| `game/` | App state, input mapping, tools (brush), edit queue, camera | everything above |
| `main.c` | SDL init and shutdown, the main loop | everything |

`sim/` and `core/` build as the static library **`pr_sim`**. It has no window, renderer or event
dependency, so tests and benchmarks run headless.

## Main loop

Fixed timestep for the simulation; rendering runs as fast as vsync allows.

```c
const double TICK_DT = 1.0 / 60.0;
const int    MAX_TICKS_PER_FRAME = 4;      // stops a spiral of death: the sim slows down instead

while (running) {
    while (SDL_PollEvent(&ev)) app_handle_event(&app, &ev);  // mouse events build brush strokes
    // app_frame():
    sim_apply_edits(world, &app.edits);    // main thread, between ticks
    accumulator += frame_time;
    int ticks = 0;
    while (accumulator >= TICK_DT && ticks < MAX_TICKS_PER_FRAME) {
        sim_tick(world, &stats);           // movement pass, then heat pass (M3)
        accumulator -= TICK_DT; ticks++;
    }
    if (ticks == MAX_TICKS_PER_FRAME) accumulator = 0;  // drop the backlog
    render_upload(); render_draw();        // upload render-dirty chunks, draw the texture
    draw_panel();                          // toolbar, HUD, brush outline
    SDL_RenderPresent(renderer);
}
```

While paused, edits are still applied each frame, so the user can paint into a frozen world.

`--frames N` runs exactly one tick per frame and quits after N frames, so a run is
deterministic. With `--screenshot file.bmp` it saves the last frame. This is used for headless
checks (for example under Xvfb). `--demo` starts from a demo scene instead of the empty box.

## World data layout

```c
#define CHUNK_SIZE      64           // must be a power of two
#define CHUNK_SHIFT     6
#define MAX_MOVE        (CHUNK_SIZE / 2)   // threading invariant (see below)

typedef uint8_t ElementId;           // 0 = ELEM_VOID; up to 255 elements

typedef struct Cell {
    ElementId element;
    uint8_t   stamp;                 // low 8 bits of the tick in which it was last updated/moved
    uint8_t   shade;                 // random at creation; colour variation; moves with the cell
    uint8_t   aux;                   // per-behaviour scratch. Liquids: bit 7 flow direction,
                                     // bits 0-6 fall speed (1/16 px/tick)
} Cell;                              // 4 bytes

typedef struct DirtyRect { int16_t min_x, min_y, max_x, max_y; } DirtyRect;  // chunk-local, inclusive

typedef struct Chunk {
    DirtyRect cur;                   // cells to process in the running tick
    DirtyRect next;                  // cells woken for the following tick
    uint8_t   render_dirty;          // needs re-upload to the texture
    uint8_t   pad[15];               // 32 bytes; M3 adds heat_active
} Chunk;

typedef struct World {
    int32_t   width, height;         // cells; multiples of CHUNK_SIZE
    int32_t   chunks_x, chunks_y;
    Cell     *cells;                 // width*height, row-major, 64-byte aligned
    float    *temp;                  // Kelvin; M3 adds a second buffer for the heat pass
    Chunk    *chunks;
    uint64_t  seed;
    uint64_t  tick;
} World;                             // M4 adds float *mass (kg per cell) for gases
```

**Why a hybrid SoA.** The movement pass mostly reads `element` and `flags`, so a 4-byte `Cell`
puts 16 cells in a cache line. The heat pass reads `element` (to look up properties) and the
temperature. Keeping temperature in its own array lets it be double-buffered and streamed. When a
cell moves, the code swaps its `Cell` *and* its `temp[cur]` entry (and `mass` from M4). See
`DECISIONS.md` D-011.

**Row-major, not chunk-major (for now).** A flat row-major array keeps indexing trivial
(`y * width + x`). If profiling in M2 shows chunk-local storage helps the cache enough, switching
would be a contained change behind `world_index()`.

**Element properties** live in parallel lookup tables indexed by `ElementId`, generated from one
definition table in `sim/elements.c`:

```c
extern const ElementDef g_elem_defs[ELEM_COUNT];   // the one definition table
extern uint8_t  g_elem_behavior[256];   // BEHAVIOR_NONE / STATIC / POWDER / LIQUID / GAS
extern uint32_t g_elem_density_q[256];  // density in g/m^3, for integer comparisons
extern uint8_t  g_elem_spread[256];     // liquid dispersion / gas step, cells per tick
extern uint16_t g_buoy_p[ELEM_COUNT][ELEM_COUNT];  // buoyancy swap probability, x/65536
```

`elements_init()` builds the lookup tables (`world_create` calls it). They have 256 entries, so
any cell's element indexes them without a bounds check. Unknown IDs, and the pseudo element
`ELEM_OUT` (returned for coordinates outside the world), behave as static walls.

## Simulation tick

```
sim_tick(world):
    stamp = tick & 0xFF
    every chunk: cur <- next, next <- empty        (wakes from edits and the last tick)
    movement pass over chunks with a non-empty cur rect, in 4 checkerboard phases;
        the first phase rotates with the tick      (M1: one thread; M2: parallel per phase)
    heat pass over heat-active chunks              (M3)
    phase changes                                  (M3; part of the heat pass)
    tick++
```

Rule details are in `SIMULATION.md`.

### Dirty rects and sleeping

Each chunk tracks a rectangle of cells that may change this tick. When a cell changes (it moves,
swaps or changes element), the code expands the *next* dirty rect of every chunk touched by that
cell's 3×3 neighbourhood (`world_wake_cell`, `world_wake_pair`). A cell that did not move but
still could (a probabilistic move that failed) keeps only itself awake (`world_keep_alive`). If
nothing changes inside a chunk during a tick, its next rect is empty and the chunk sleeps until
something wakes it: a neighbour's change, a player edit, or (M3) heat.

### Parallel movement: 4-phase checkerboard *(M2)*

Chunks are grouped by `(cx & 1, cy & 1)` into 4 phases. The scheduler runs the phases one after
another, with a barrier between them. Within a phase, all dirty chunks of that group are processed
in parallel, one job per chunk.

During its phase, a worker owns its chunk and may also read and write up to `MAX_MOVE` (32) cells
into the neighbouring chunks. Two chunks in the same phase always have one full chunk (64 px)
between them. Each worker reaches at most 32 px into that gap, so their regions never overlap.

**Invariant: no single tick may move a cell more than 32 px from its start, and no rule may read
further than 32 px from the cell it updates**, in any direction. Liquid spreading and lookahead,
gas drift and any future projectile must clamp to this. Debug builds assert it on every swap.

Dirty-rect updates that land in a neighbouring chunk go through either SDL atomics (min/max via
compare-and-swap) or a per-worker deferred list merged after the phase. Pick whichever benchmarks
better and record it in `DECISIONS.md`.

### Determinism

Running the same world with the same seed and the same edit stream gives a bit-identical world,
whatever the thread count. This requires:

- Each chunk job seeds its RNG from `hash(world.seed, world.tick, cx, cy)`.
- The phase order depends only on the tick.
- A cell is updated only by the job that owns its chunk in that phase, in a fixed scan order.
- Floating-point code (heat) is written so that per-cell results do not depend on scheduling
  (Jacobi double-buffering, no reductions whose order depends on threads).

## Player edits

The brush never writes into the world directly. `game/app.c` turns mouse events into
`Edit { x0, y0, x1, y1, radius, shape, element, only_void }` stroke segments:

- button down: start a stroke with a dot;
- each motion event: a segment from the previous position to the new one;
- button up: a final segment to the release point, then the stroke ends;
- button held still: the same dot is re-applied every frame, so liquids pour.

Segments go into an `EditQueue` (fixed capacity 256; if it is full, it is flushed first).
`sim_apply_edits()` rasterises them on the main thread between ticks:

- Long segments are split into pieces of at most `max(4, radius)` cells, so each piece's bounding
  box stays small.
- A cell is covered if its centre is within `radius + 0.5` of the segment (circle brush, a
  capsule) or the swept square touches it (square brush, a Liang–Barsky test). Radius 0 is a
  single pixel, and fast strokes leave no gaps.
- Each covered cell gets the element, a random `shade`, a random flow direction, the element's
  spawn temperature, and a stamp that guarantees it is simulated in the next tick.
  `only_void` (Shift) skips non-void cells.
- The bounding box is woken (movement and render).

## Rendering

- One `SDL_Texture` (`SDL_PIXELFORMAT_RGBA8888`, `SDL_TEXTUREACCESS_STREAMING`), the same size as
  the world, or the same size as the visible window into the world once a camera exists (M2).
- A lookup table `lut[element][shade]` holds the final colour: the base colour varied by shade,
  then blended over the void colour by the element's alpha (gases, water). Conversion is one
  table load per cell.
- For each chunk with `render_dirty`, convert its cells. If more than a quarter of the chunks are
  dirty, upload the whole texture once; otherwise upload each dirty chunk's rect
  (`SDL_UpdateTexture` with a sub-rect).
- Draw the texture with `SDL_SCALEMODE_NEAREST`, scaled to fit the area right of the toolbar with
  the aspect ratio kept. The scale is rounded down to an integer when it is ≥ 1, so all pixels are
  the same size.
- Overlays (temperature, pressure) are alternative colour mappings in the same conversion step.
- If we need shaders (glow, lighting, smooth gas rendering), move to the SDL3 GPU API. Record that
  in `DECISIONS.md`.

## UI

A small hand-rolled immediate-mode UI (`ui/ui.c`), drawn with `SDL_RenderFillRect` and SDL3's
built-in 8×8 debug font (`SDL_RenderDebugText`):

```c
if (ui_button(&ui, "Water", rect, selected == ELEM_WATER)) selected = ELEM_WATER;
```

The UI consumes mouse input over its widgets, so clicking a button never paints the world
underneath it. Revisit this approach (Nuklear, microui, or SDL_ttf for text) once the UI grows
beyond toolbars and HUDs (`DECISIONS.md` D-009).

## Coordinates

- World coordinates: integer cells, origin at the **top-left**, **+y is down** (matches memory
  layout and SDL). "Up" in the rules means `y - 1`.
- Screen → world: `world = (screen - viewport_origin) / scale (+ camera offset in M2)`.

## Memory

All large buffers are allocated once at world creation with 64-byte alignment
(`SDL_aligned_alloc`). Nothing is allocated per tick. Example budget at 4096 × 2048 (8.4 M
cells): cells 32 MB + temperature 2 × 32 MB + mass 32 MB ≈ 128 MB.
