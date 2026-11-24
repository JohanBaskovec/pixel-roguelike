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
const int    MAX_TICKS_PER_FRAME = 4;      // catching up after a hiccup

while (running) {
    while (SDL_PollEvent(&ev)) app_handle_event(&app, &ev);  // mouse events build brush strokes
    // app_frame():
    sim_apply_edits(world, &app.edits);    // main thread, between ticks
    accumulator += frame_time;
    int ticks = 0;
    int max_ticks = MAX_TICKS_PER_FRAME;
    while (accumulator >= TICK_DT && ticks < max_ticks) {
        sim_tick(world, &stats);           // movement pass, then heat pass (M3)
        if (that tick took longer than TICK_DT) max_ticks = 1;  // cannot catch up: slow down
        accumulator -= TICK_DT; ticks++;
    }
    if (ticks == max_ticks && accumulator >= TICK_DT) accumulator = 0;  // drop the backlog
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
    uint8_t   aux;                   // per-behaviour scratch. Liquids and powders: bit 7 flow
                                     // direction, bits 0-6 fall speed (1/16 px/tick). Gases: a
                                     // current (bits 3-7 speed, bits 0-2 direction)
} Cell;                              // 4 bytes

typedef struct DirtyRect { int16_t min_x, min_y, max_x, max_y; } DirtyRect;  // chunk-local, inclusive

typedef struct Chunk {
    DirtyRect cur;                   // cells to process in the running tick
    DirtyRect next;                  // cells woken for the following tick
    float     temp_lo, temp_hi;      // range of physical temperatures in the chunk
    uint8_t   render_dirty;          // needs re-upload to the texture
    uint8_t   head_dirty, head_dirty_next;   // relabel for water pressure (this / next tick)
    uint8_t   heat_active, heat_active_next; // run the heat pass (this / next tick)
    uint8_t   heat_check;            // cells moved in: compare temperatures next tick
    uint8_t   pad[2];                // 32 bytes
} Chunk;

typedef struct World {
    int32_t   width, height;         // cells; multiples of CHUNK_SIZE
    int32_t   chunks_x, chunks_y;
    Cell     *cells;                 // width*height, row-major, 64-byte aligned
    float    *temp;                  // stored temperature, K (holds latent heat; see SIMULATION.md)
    float    *temp_next;             // heat pass scratch (Jacobi)
    Chunk    *chunks;
    uint64_t  seed;
    uint64_t  tick;
    uint8_t  *piece;                 // water pressure: piece id of still liquid within its chunk
    JobPool  *jobs;                  // not owned; NULL runs single-threaded
    void     *sim_scratch;           // owned; per-phase job contexts
    struct PressureState *pressure;  // owned; water-pressure pieces, union-find, body tops
    GasMix   *gas;                   // per cell: kg of each gas species, gas cells only (D-035)
    float    *rho;                   // per cell: gas density at 1 atm, for buoyancy (per tick)
} World;
```

**Why a hybrid SoA.** The movement pass mostly reads `element` and `flags`, so a 4-byte `Cell`
puts 16 cells in a cache line. The heat pass reads `element` (to look up properties) and the
temperature. Keeping temperature in its own array lets it be double-buffered and streamed. A gas
cell's mixture (`GasMix`: 5 floats, 20 bytes) is its own array too, read only for gas cells. When
a cell moves, the code swaps its `Cell`, its `temp` entry, its `gas` and its `rho`. See
`DECISIONS.md` D-011 and D-035.

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

**Free particles (M4b).** `World.particles` is a list of up to `PARTICLE_MAX` (131072)
`Particle`s: position and velocity in floating point (cells, cells per tick), the `Cell` it
carries and its stored temperature (24 bytes; 3 MB). A flying particle is not on the grid: the
cell it left is empty. See `SIMULATION.md` → Free particles.

**Creatures (M4c).** `World.creatures` holds up to `CREATURE_MAX` (64) `Creature`s: position,
velocity, walking and breathing state, health. Their bodies are `ELEM_BODY` cells on the grid.
See `SIMULATION.md` → Creatures.

## Simulation tick

```
sim_tick(world):
    creatures: move, breathe, heat, in list order (serial; M4c)
    stamp = tick & 0xFF
    every chunk: cur <- next, next <- empty        (wakes from edits and the last tick)
    heat: conduction over heat-active chunks and their neighbours (parallel, Jacobi into
        temp_next), then write-back and phase changes (parallel); cells that changed phase are
        added to this tick's rects
    water pressure: relabel chunks whose still liquid changed (parallel), rebuild bodies with a
        union-find (serial); chunks whose body top rose are added to this tick's rects
    gas densities: each chunk with a non-empty cur rect recomputes World.rho, its gas cells'
        densities at 1 atm, for buoyancy (parallel)
    movement pass over chunks with a non-empty cur rect, in 4 checkerboard phases;
        the first phase rotates with the tick; the chunks of a phase run in parallel;
        after each phase, the phase's water-pressure transfer requests are applied (serial;
        deep side openings launch jets of free particles), then the particles the phase's jobs
        launched (splashes) are added to the particle list, in job order
    free particles: gravity and one tick of flight each, in list order (serial); landed ones go
        back onto the grid                         (M4b)
    tick++
```

Rule details are in `SIMULATION.md`.

### Dirty rects and sleeping

Each chunk tracks a rectangle of cells that may change this tick. When a cell changes (it moves,
swaps or changes element), the code expands the *next* dirty rect of every chunk touched by that
cell's 3×3 neighbourhood (`ctx_wake` inside the movement pass, `world_wake_rect` outside it). A
cell that did not move but still could (a probabilistic move that failed) keeps only itself awake
(`keep_alive`). If nothing changes inside a chunk during a tick, its next rect is empty and the
chunk sleeps until something wakes it: a neighbour's change, a player edit, or (M3) heat.

One rectangle per chunk is coarse: a diagonal line of woken cells (a bullet's trail) makes the
whole chunk awake. Finer tracking is proposed (`PERFORMANCE.md` → Proposed next steps).

### Parallel movement: 4-phase checkerboard

Chunks are grouped by `(cx & 1, cy & 1)` into 4 phases. The scheduler runs the phases one after
another, with a barrier between them. Within a phase, all dirty chunks of that group are processed
in parallel, one job per chunk.

During its phase, a worker owns its chunk and may also read and write up to `MAX_MOVE` (32) cells
into the neighbouring chunks. Two chunks in the same phase always have one full chunk (64 px)
between them. Each worker reaches at most 32 px into that gap, so their regions never overlap.

**Invariant: no single tick may move a cell more than 32 px from its start, and no rule may read
further than 32 px from the cell it updates**, in any direction. Liquid spreading and lookahead
and every gas rule clamp to this. Debug builds assert it on every swap. What runs serially is
not bound by it: creatures (start of the tick), water-pressure transfers (after each phase) and
free particles (end of the tick), which is how a bullet crosses 300 cells in a tick.

**How a phase runs** (`sim_tick` in `src/sim/sim.c`):
1. List the phase's chunks with a non-empty dirty rect. Give each a `TickCtx`: its chunk, its RNG,
   its stats, and wake buffers for the 3×3 chunks around it.
2. `jobs_run()` updates them in parallel, one job per chunk. A job works on a copy of its
   `TickCtx` on its own stack, because updating the contexts in place, side by side in one array,
   made threads fight over shared cache lines (it cost half the speed-up).
3. After the barrier, the main thread merges every job's wake buffers into the chunks' `next`
   rects and `render_dirty` flags. Min/max and OR do not depend on order, so neither does the
   result (D-019).

Wakes never write to another chunk's metadata during a phase, so no atomics or locks are needed
in the hot path.

**Thread pool** (`src/core/jobs.c`): a fork-join pool on SDL threads. `jobs_run(pool, count, fn,
user)` runs `fn(user, i)` for every `i`, with workers claiming indices from an atomic counter. The
calling thread works too, and the call returns when all jobs are done. With a NULL pool, or a
single thread, jobs run in order on the caller. The world uses the pool set with
`world_set_job_pool()`; the game creates one thread per logical core (`--threads N` to change).

### Determinism

Running the same world with the same seed and the same edit stream gives a bit-identical world,
whatever the thread count. This requires:

- Each chunk job seeds its RNG from `hash(world.seed, world.tick, cx, cy)`.
- The phase order depends only on the tick.
- A cell is updated only by the job that owns its chunk in that phase, in a fixed scan order.
- Floating-point code (heat) is written so that per-cell results do not depend on scheduling
  (Jacobi double-buffering, no reductions whose order depends on threads).
- Free particles are launched in job order and updated serially in list order (removal swaps
  the last particle in, deterministically). Tested with jets on 1 and 4 threads.
- Creatures update serially in list order, with randomness from a hash of seed, tick and their
  id. Tested with three wandering, breathing creatures on 1 and 4 threads.

## Player control (M6a)

`game/app.c` drives the player creature between ticks, on the main thread, before each
`sim_tick`: the walking direction and jump from the keyboard (scancodes, so the physical keys
are the same on any layout), the facing from the mouse, and `rifle_update()` with the trigger.
Shots add particles; their flight runs in the tick's serial particle step. The camera follows the
player at zoom 1 unless the user pans. The rifle and bullet streaks are drawn over the world
texture (they are not cells).

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

- One `SDL_Texture` (`SDL_PIXELFORMAT_XRGB8888`, `SDL_TEXTUREACCESS_STREAMING`), the same size as
  the world (32 MB at 4096 × 2048), plus a world-sized staging buffer in system memory.
- A lookup table `lut[element][shade]` holds the final colour: the base colour varied by shade,
  then blended over the void colour by the element's alpha (water). Conversion is one table load
  per cell, except gas cells, whose colour is mixed from their species and pressure
  (`gas_color`), and glowing cells.
- Each frame, only render-dirty chunks **inside the camera view** are converted (in parallel on
  the job pool) and uploaded. Dirty chunks outside the view keep their flag until they come into
  view. Uploads are one `SDL_UpdateTexture` per chunk row, spanning that row's dirty chunks: a few
  large uploads are much cheaper than many small ones (D-020).
- Free particles (M4b) are drawn over the grid: after converting, each particle in view writes
  its cell's colour into the staging buffer. The chunks particles are on are converted and
  uploaded every frame, and so are the chunks they were on the frame before (erasing them).
- The visible part of the texture is drawn into the view (the window right of the toolbar).
  Zoomed in (zoom ≥ 1), sampling is nearest-neighbour, so pixels stay sharp. Zoomed out, it is
  linear, which reduces flicker from skipped pixels.

## Camera

- The camera is the world position of the view's top-left corner (`cam_x`, `cam_y`), plus a zoom
  level from a fixed list: 1/4, 1/3, 1/2, 1, 2, 3, 4, 6, 8 screen pixels per cell. Integer
  levels keep every cell the same size on screen.
- It is clamped so the view stays inside the world (a world smaller than the view is centred),
  and snapped to whole screen pixels so the image does not shimmer while panning.
- Controls: middle-drag or arrows to pan (WASD too when there is no player), Ctrl+wheel to zoom
  around the cursor, Home to fit the whole world, `F` to follow the player again. The game starts
  at zoom 1, following the player.
- Overlays are alternative colour mappings in the same conversion step: pressure view (`P`,
  depth below the body's top) and temperature view (`T`). While one is on, everything visible is
  redrawn every frame.
- Hot matter glows: above ~480 °C the cell's colour is blended toward a blackbody colour (dull red
  → orange → white). Chunks with glowing cells are redrawn when their temperatures change.
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
- Screen → world: `world = cam + (screen - view_origin) / zoom`.

## Memory

All large buffers are allocated once at world creation with 64-byte alignment
(`SDL_aligned_alloc`). Nothing is allocated per tick. Budget at 4096 × 2048 (8.4 M cells) today:
cells 32 MB + temperature 2 × 32 MB + gas mixtures 160 MB + gas densities 32 MB + piece labels
8 MB + water-pressure tables ≈ 14 MB, plus the renderer's 32 MB staging buffer and 32 MB texture.
