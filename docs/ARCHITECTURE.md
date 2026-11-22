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

while (app.running) {
    input_poll(&app);                      // SDL events -> input state
    game_update(&app);                     // tools read input and push edits into the edit queue
    accumulator += frame_time;
    int ticks = 0;
    while (accumulator >= TICK_DT && ticks < MAX_TICKS_PER_FRAME) {
        if (!app.paused || app.step_once) {
            sim_apply_edits(world, &app.edits);   // main thread, between ticks
            sim_tick(world);                      // movement pass, then heat pass (M3)
        }
        accumulator -= TICK_DT; ticks++;
    }
    if (ticks == MAX_TICKS_PER_FRAME) accumulator = 0;  // drop the backlog
    render_world(&app.renderer, world);    // upload dirty chunks, draw the texture
    ui_frame(&app);                        // toolbar, HUD, brush outline
    SDL_RenderPresent(renderer);
}
```

While paused, edits are still applied each frame, so the user can paint into a frozen world.

## World data layout

```c
#define CHUNK_SIZE      64           // must be a power of two
#define CHUNK_SHIFT     6
#define MAX_MOVE        (CHUNK_SIZE / 2)   // threading invariant (see below)

typedef uint8_t ElementId;           // 0 = ELEM_VOID; up to 255 elements

typedef struct Cell {
    ElementId element;
    uint8_t   flags;                 // bit 0: clock (tick parity when last updated); others reserved
    uint8_t   shade;                 // random at creation; colour variation; moves with the cell
    uint8_t   aux;                   // per-element scratch (e.g. liquid flow direction, lifetime)
} Cell;                              // 4 bytes

typedef struct Chunk {
    // dirty rect for the current tick and for the next tick, in chunk-local coordinates
    int16_t cur_min_x, cur_min_y, cur_max_x, cur_max_y;
    int16_t next_min_x, next_min_y, next_max_x, next_max_y;
    uint8_t render_dirty;            // needs re-upload to the texture
    uint8_t heat_active;             // has temperature gradients above epsilon (M3)
} Chunk;                             // padded/aligned to avoid false sharing (M2)

typedef struct World {
    int32_t   width, height;         // cells; multiples of CHUNK_SIZE
    int32_t   chunks_x, chunks_y;
    Cell     *cells;                 // width*height, row-major, 64-byte aligned
    float    *temp[2];               // Kelvin; double-buffered for the heat pass (M3); temp[cur] is live
    float    *mass;                  // kg per cell; added in M4 for gases
    Chunk    *chunks;
    uint64_t  seed;
    uint64_t  tick;
} World;
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
extern const uint8_t  g_elem_behavior[256];   // BEHAVIOR_NONE / STATIC / POWDER / LIQUID / GAS
extern const uint16_t g_elem_density_q[256];  // quantised density used for comparisons in movement rules
extern const float    g_elem_density[256];    // kg/m^3
extern const float    g_elem_heat_cap[256];   // J/(kg*K)
extern const float    g_elem_conduct[256];    // W/(m*K)
extern const uint32_t g_elem_color[256];      // base RGBA
// ... phase-change thresholds, etc.
```

## Simulation tick

```
sim_tick(world):
    world.clock ^= 1
    movement pass over dirty chunks      (M1: one thread, chunk order; M2: 4-phase checkerboard)
    heat pass over heat-active chunks    (M3)
    phase changes                        (M3; part of the heat pass)
    swap chunk dirty rects: cur <- next, next <- empty
    tick++
```

Rule details are in `SIMULATION.md`.

### Dirty rects and sleeping

Each chunk tracks a rectangle of cells that may change this tick. When a cell changes (it moves,
swaps or changes element), the code expands the *next* dirty rect of every chunk touched by that
cell's 3×3 neighbourhood. If nothing changes inside a chunk during a tick, its next rect is empty
and the chunk sleeps until something wakes it: a neighbour's change, a player edit, or (M3) heat.

### Parallel movement: 4-phase checkerboard *(M2)*

Chunks are grouped by `(cx & 1, cy & 1)` into 4 phases. The scheduler runs the phases one after
another, with a barrier between them. Within a phase, all dirty chunks of that group are processed
in parallel, one job per chunk.

During its phase, a worker owns its chunk and may also read and write up to `MAX_MOVE` (32) cells
into the neighbouring chunks. Two chunks in the same phase always have one full chunk (64 px)
between them. Each worker reaches at most 32 px into that gap, so their regions never overlap.

**Invariant: no single tick may move a cell more than 32 px from its start**, in any direction.
Liquid spreading, gas drift and any future projectile must clamp to this. Debug builds assert it.

Dirty-rect updates that land in a neighbouring chunk go through either SDL atomics (min/max via
compare-and-swap) or a per-worker deferred list merged after the phase. Pick whichever benchmarks
better and record it in `DECISIONS.md`.

### Determinism

Running the same world with the same seed and the same edit stream gives a bit-identical world,
whatever the thread count. This requires:

- Each chunk job seeds its RNG from `hash(world.seed, world.tick, cx, cy)`.
- A cell is updated only by the job that owns its chunk in that phase, in a fixed scan order.
- Floating-point code (heat) is written so that per-cell results do not depend on scheduling
  (Jacobi double-buffering, no reductions whose order depends on threads).

## Player edits

The brush never writes into the world directly. `game/tools.c` turns mouse input into
`Edit { shape, x0, y0, x1, y1, radius, element }` records (a stroke segment from the previous
mouse position to the current one). It pushes them into an `EditQueue`. `sim_apply_edits()`
rasterises them on the main thread between ticks:

- For each covered cell: set `element`, randomise `shade`, set the temperature to the element's
  default temperature (or keep it, depending on the tool), and set the clock bit so the cell does
  not move in the same tick.
- Expand the dirty rects (movement and render) of the affected chunks.

Painting is pixel-precise. Brush radius 0 means a single pixel. A stroke segment is rasterised as
a capsule, which is the circle swept along the segment, so fast mouse moves leave no gaps.

## Rendering

- One `SDL_Texture` (`SDL_PIXELFORMAT_RGBA8888`, `SDL_TEXTUREACCESS_STREAMING`), the same size as
  the world, or the same size as the visible window into the world once a camera exists (M2).
- For each chunk with `render_dirty`, convert its cells to RGBA:
  `color = shade_variation(g_elem_color[e], shade)`. Gases are blended over the void colour at a
  per-gas alpha. Then upload that rect (`SDL_UpdateTexture` with a sub-rect; M2 may batch rows with
  `SDL_LockTexture` instead).
- Draw the texture with `SDL_SCALEMODE_NEAREST`, scaled to fit the window with the aspect ratio
  kept.
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
