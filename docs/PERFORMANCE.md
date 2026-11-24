# Performance

Performance is a design constraint, not a polish step. This doc defines the budgets, the
techniques we rely on, how to measure, and the benchmark log.

## Reality check

"Millions of cells" means millions of cells **in the world**, with only the **active** ones
simulated each tick. Noita works the same way: settled rock, still water and stratified gas sleep.
A naive pass over every cell, every tick, costs roughly 5–20 ns per cell. That is 40–160 ms per
tick single-threaded for an 8 M cell world, which is far over budget. Sleeping chunks and
multithreading together make the target reachable.

## Budgets

The frame budget at 60 FPS is 16.6 ms. Target hardware: a mid-range desktop from 2020 or later
with 6–8 cores.

| Phase | Budget per tick | Notes |
|---|---|---|
| Movement pass | ≤ 6 ms | Across all worker threads |
| Heat pass (M3) | ≤ 3 ms | Active chunks only; may run every N ticks |
| Edit application | ≤ 0.5 ms | Large brushes on the main thread |
| Texture conversion + upload | ≤ 2 ms | Dirty chunks only |
| UI + present | ≤ 1 ms | |

| Milestone | World | Target |
|---|---|---|
| M1 | 1024 × 768 (0.8 M cells), single-threaded | 60 ticks/s with the whole box churning |
| M2 | 4096 × 2048 (8.4 M cells), multithreaded | 60 ticks/s with ≈ 2 M active cells (desktop, not yet verified; see ROADMAP) |

**The scene to hold at 60 ticks/s** (the user's target, 2026-11-23; not met yet): a firefight on
screen (about 520 000 cells in view on a 1920 × 1080 screen at the default zoom ×2, at 5 cm per
cell: D-037, D-038), with tens of bullets
in flight, enemies shooting back, the player jumping into water, walls coming apart cell by
cell, and the air stirred and heated all around; later smoke grenades, rockets and acid. Solids,
powders, liquids, bullets and bodies are cheap enough already; **gas is what does not fit**
(Proposed next steps, below). The tables above predate gases with pressure and heat: the gas
work has no budget line of its own yet, and today it takes most of the tick.

## Techniques we rely on

Ordered by impact.

1. **Sleeping chunks and dirty rects.** Do no work where nothing changes. This is the biggest win
   by far. Every new rule must leave settled cells in a state where they stop marking themselves
   dirty. For example, a gas cell that matches its neighbours within the tolerances must not keep
   itself awake.
2. **Multithreading.** A 4-phase checkerboard over chunks (`ARCHITECTURE.md`). Use one worker per
   core minus one. Jobs are whole chunks, so per-job overhead stays small.
3. **Compact cells.** A 4-byte `Cell` gives 16 cells per cache line. Temperature lives in a
   separate array so the movement pass does not drag it through the cache except when swapping.
4. **Table-driven properties.** Element properties are flat arrays indexed by element ID, small
   enough to stay in L1. Do not chase pointers per cell.
5. **Branch-light inner loops.** Skip `STATIC` and `NONE` with one table lookup. Dispatch the
   remaining behaviours through a `switch`, never through function pointers.
6. **Integer movement logic** for liquids and powders: quantised densities and an integer RNG,
   with floating point only for convection. Gas rules are floating point throughout (mixtures,
   pressure, densities), which is one reason gas cells cost 10 times more than the others.
7. **Upload only dirty chunks** to the texture.
8. **No allocation after startup.** All buffers are allocated once and aligned to 64 bytes.
   Per-chunk metadata is padded to cache lines to avoid false sharing between workers.
9. **Heat only on heat-active chunks** (and their neighbours). Running it every N ticks with a
   scaled dt is an idea, not implemented: it runs every tick.

Planned experiments, decided by data only:
- A permanent 1-cell wall ring at the world edge, so neighbour reads need no bounds check.
- Chunk-major memory layout instead of row-major.
- Full SoA for `Cell`.
- ~~Atomics vs per-worker deferred lists for cross-chunk dirty marking~~: per-job buffers (D-019).
- SIMD for the heat pass (see Proposed next steps: the gas field pass would include it).
- `SDL_LockTexture` vs `SDL_UpdateTexture` for uploads.

## Measuring

- **`pr-bench`** (headless) builds a deterministic scenario: a world filled with a churning mix of
  water and three gases over a dirt-and-rock floor (`--fill F` sets how much of the space above
  the floor is filled; `--threads N` the thread count, default all cores). It runs a warm-up,
  then N ticks, and prints:
  - ms/tick at p50, p99 and max
  - updated cells per tick
  - **ns per updated cell** (the main number to watch)
  - thread count, world size, build type, compiler
- **In-game HUD timers**, built on `SDL_GetPerformanceCounter` and averaged over half a second:
  Sim (per tick), Draw (convert + upload of visible dirty chunks, per frame), Frame (all work per
  frame, excluding the wait for vsync).
- **Races:** build with `-DPR_TSAN=ON` (ThreadSanitizer) and run the multithreaded tests
  (`ctest -R thread_count`) and a few `pr-bench --threads 4` ticks after any threading change.
- **Tests' speed:** each test is its own CTest test (`ctest -j4` runs them in parallel), the
  sanitizer build compiles at `-O1`, and tests use the smallest world and fewest ticks that show
  their behaviour. Release: 75 s → 12 s; ASan + UBSan: about 14 min → 1 min (4 cores, M6a).
  With 86 tests (after gas mixtures): about 20 s and 2 min.
- **Noise.** On this VM, wall-clock times of the same build vary by about ±10% from run to run,
  and the play-test scenes are chaotic: the same build with another world seed varies by up to 15%
  (firing in the room, callgrind: 40.9 / 47.3 / 41.8 / 41.3 G instructions for seeds 5 to 8). For
  a change worth less than that, compare instruction counts under callgrind over 4 seeds, paired
  by seed, then confirm with single-thread wall-clock runs, old and new alternately (3 each). Both
  are needed: an equalisation change cut instructions by 11–15% on every seed and gained no time
  (Tried and dropped).
- **External profilers:** `perf` / Hotspot (Linux), Instruments (macOS), and Superluminal or VS
  Profiler (Windows). Use a `RelWithDebInfo` build. Tracy may be added later; it needs a
  `DECISIONS.md` entry because it is a dependency.

Rules
- Quote only Release or RelWithDebInfo numbers, never Debug.
- Compare before and after on the same machine, with the same scenario and seed, in back-to-back
  runs.
- Every change to `src/sim/` hot code adds a line to the benchmark log below.

## Proposed next steps (not implemented)

Written 2026-11-23, after gas mixtures (D-035) made the two play-tested cases only 1.2–2× faster.
Nothing in this section is built. Numbers marked *measured* come from this machine (4-core VM,
gcc 13.3 `-O3`); everything else is an estimate, and two earlier estimates in this project were
2–4× too optimistic, so each step should be prototyped and measured before the old code is
removed. The proposed decision is D-036. Cells stay 5 cm (D-037), so the speed has to come from
here and from simulating only near the player (ROADMAP M6b).

### Where the time goes today (measured)

Two cases in a 1024 × 1024 room of air, averaged over 10 s (600 ticks):

| | Firing into the air | CO2 ball, radius 100 |
|---|---|---|
| Gas cells visited per tick | 125 000 | 127 000 |
| ... that leave at once, exactly like their neighbours | 32 000 (26%) | 13 000 (10%) |
| ... that leave after the tolerance checks (about 40 ns each) | 60 000 (48%) | 31 000 (24%) |
| ... that may do something | 32 000 (26%) | 83 000 (65%) |
| ... that do something (diffuse, equalise pressure, move) | about 12 000 | about 70 000 |
| Time per tick, 1 thread / 4 threads, at 10 s | 34 / 11.5 ms | 31 / 11–12 ms |

- **Firing:** three visits in four find nothing to do. This was first put down to the chunk's
  single bounding rectangle (a bullet's diagonal trail waking the whole chunk). Tracking per
  16 × 16 tile (step 2, done) showed otherwise: the tiles cut the processed area by only 8% in
  this case. The awake region is a broad patch of stirred air, and most of its cells are
  neighbours of cells that acted (every exchange wakes the 3 × 3 cells around both ends), which
  then find themselves within tolerance. The heat pass works on whole chunks: the heat-active
  chunks and their four neighbours.
- **CO2 ball:** most visited cells are really diffusing. The cost there is the rule itself: one
  random direction per tick, exchanging in place, with branches and a random number per cell.
- **Profile, firing:** movement pass 65% (of which pressure equalisation 8%), heat pass 25%, gas
  density pass 6%. After step 2 (callgrind, 1 thread, instructions): the gas rule 55%, of which
  the sleep check (`gas_is_mobile`) is about half, and within it the composition comparison with
  each neighbour (`gas_mix_differs`) is the largest single line (8% of the tick); the heat pass
  20% (`compute_chunk` 16%, `apply_chunk` 7%).
- **Threads:** 4 threads give 2.6–3×. Few chunks are awake per checkerboard phase, and
  water-pressure transfers and free particles run serially.
- A cell within tolerance of its neighbours costs about 40 ns per visit; an active one 100 ns and
  more. A lean falling-sand rule costs 5–10 ns.

### 1. Compute gas as a field (on hold after measuring it in the game, 2026-11-24)

**Measured in the game.** One step of the exchange below (pressure flow, diffusion and
conduction between neighbours, scale 1 only), timed over the cells it would have to compute: the
awake tiles and a ring of one tile around them (the cells beyond the ring count as walls, as in
the heat pass), on the game's data (one 20-byte mixture per cell), one thread, not applied:

| | Cells per tick | Shadow field step | Whole tick today |
|---|---|---|---|
| Firing into the air, 10 s | 204 000 | 11.2 ms (55 ns per cell) | 16.9 ms, of which gas and heat about 12.7 |
| CO2 ball, 10 s | 172 000 | 9.7 ms (56 ns per cell) | about 17 ms |

So, on today's data layout, the neighbour exchange alone costs about as much as everything gas
and heat do today, before buoyancy, currents, condensation, long-range pressure and the sleep
bookkeeping are added. One array per species and vectorised loops (the prototype's 28 ns per cell)
would halve it, but that changes every user of the gas data. The estimate from all this: between
1.4× faster and somewhat slower than today for the busy cases, for a rewrite of the gas rules and
tests and 20 more bytes per cell for double buffering. On hold. The prototype's numbers below
compared a fully active room with today's sparse one, which made the field pass look like the
largest gain; the table above is the like-for-like comparison.

**Today** the storage is a field (each gas cell holds a mass of each species and a temperature),
but most movement is not: buoyancy, currents and spreading trade whole cells' contents
(`swap_cells`, `gas_lift`), vacuum is a separate element that gas splits into, and diffusion and
pressure pick one random neighbour per tick and settle with it in place.

**Proposed:** gas cells never move. Each tick, for every face between two neighbouring cells, the
amount of each species and of heat that crosses it is computed from the previous tick's values
(pressure difference, mole-fraction differences, temperature difference, and the density
difference across horizontal faces for buoyancy), then all faces are applied at once. Each face
is computed once and applied to both cells, so mass and energy are conserved exactly. Vacuum is a
gas cell holding nothing. This is how the heat pass already works.

- **No randomness, no order dependence.** The result is the same for any thread count without
  the checkerboard, so gas can run as a plain parallel loop over tiles or rows (the checkerboard
  stays for liquids and powders).
- **It replaces** the gas rules in `update_gas()`, the gas part of the heat pass, and the density
  pass. Species masses move from one 20-byte struct per cell to one array per species, so each
  loop reads only what it needs and the compiler can vectorise it.
- **All four neighbours every tick** instead of one at random: gas settles sooner, so regions
  sleep sooner.
- **Other matter:** liquid, powder, particles and bodies entering a gas cell still push its gas
  into the cells around (as now, `world_gas_region`). Gas "grain" (the per-cell shade) becomes
  noise in the renderer.

**Measured** with `bench/proto_gas_field.c` (5 species and temperature, pressure flow, diffusion
and conduction; 1024 × 1024 cells, every one active, every tick):

| Build | 1 thread | 4 threads |
|---|---|---|
| Baseline x86-64 (SSE2) | 28–31 ns per cell, 29–33 ms per tick | 8.1–8.4 ns per cell, 8.5–8.8 ms per tick |
| `-mavx2 -mfma` | 18.5 ns per cell | 5.5 ns per cell, 5.7 ms per tick |

So a whole 1024 × 1024 room, fully active, fits in a tick on 4 threads, where today 125 000
visited cells cost 11.5 ms. For the firing case that is roughly 4 ms on 1 thread and 1 ms on 4
for the same cells, before any sleeping. The prototype has no buoyancy term, no sleeping, no
walls beyond a mask and no other matter, so the real pass will cost more; a first naive version
of it, which the compiler could not vectorise, cost 88 ns per cell. The loops must stay simple
(one array per species, no aliasing) and that must be checked on GCC, Clang (24 ns per cell here)
and MSVC. AVX2 needs the pass compiled twice and chosen at start-up with `SDL_HasAVX2()` (no new
dependency).

**What it does not solve by itself.** Exchange between neighbours moves pressure about one cell
per tick: a 2:1 pressure step across a 6 m room would take minutes to even out (it takes about
5 s today, thanks to the jumps of up to 16 cells). The field pass needs a long-range part:
- the simple one: also relax pressure between coarse blocks (8 × 8 cells, then chunks), scaling
  the cells of a block, as a multigrid solver does; or
- step 3, a velocity field, which carries pressure at a chosen speed of sound.

Buoyancy is the other gap. As an exchange across horizontal faces (a fraction of each cell moving
up, the same of the other moving down) it is a diffusion where the stack is unstable: a heavy
front advances as the square root of time, and a CO2 ball 300 cells above the floor would take
minutes to land. Today's rules move whole cells (swaps, `gas_lift`), which is advection: a cell
sinks at a speed. A field model needs a velocity (step 3) for buoyancy to move gas as a body, or
keeps today's swaps for it.

### 2. Track activity per tile, not per chunk rectangle (done, 2026-11-24)

Each dirty rect also keeps a bit per 16 × 16 tile (16 per chunk) for where it was woken
(`DirtyRect.tiles`); the movement pass and the gas density pass process only the rect's cells
inside those tiles. Heat stays per chunk.

- No change to the physics. Wakes merge by OR, so the result still does not depend on the thread
  count.
- **Estimated** up to 2× for the firing case. **Measured** (4 threads, average over 10 s, two
  runs each, alternately): firing in a 1024 × 1024 room 6.31 / 6.51 ms against 6.64 / 6.86 ms
  (about 5%); the CO2 ball 6.94 / 6.89 against 7.13 / 6.83 (no change); firing in the default
  level, and `pr-bench`, within this VM's noise. Cells visited: 8% fewer in the room, 12–18% in
  the level. The estimate assumed the awake cells were thin bullet trails inside big rectangles;
  they are mostly the neighbours of active cells (see "Where the time goes"). With step 1 the
  mask is needed anyway, to skip settled tiles.
- **Tried with it and dropped:** caching each gas cell's moles next to its density, for the
  sleep checks (1% fewer instructions for 4 bytes per cell, 34 MB at 4096 × 2048); computing a
  gas cell's heat capacity and conductivity in the heat pass only where a neighbour differs in
  temperature (no change when firing: the heat-active chunks are not uniform). Kept: the sleep
  check reads the neighbours' densities from the per-tick cache (`World.rho`) instead of
  recomputing them (about 5% fewer instructions when firing).
- **Found on the way:** a water-pressure transfer could carry a lone air bubble from under a
  pool's surface to the far end of the body (it traded places with the moving water), here into
  the vacuum of a sealed barometer tube, which then never filled. Fixed: no transfer when the gas
  at the opening has no gas around it to be compressed into (SIMULATION → Liquid pressure).

### 3. Give air a velocity

A wind velocity per small block (4 × 4 cells, 20 cm, to be tuned), accelerated by pressure
differences and buoyancy, slowed by walls and friction, which carries each cell's gas and heat
along (the field pass's bulk-flow term then uses this velocity instead of the local pressure
difference).

- **What it gives:** plumes and convection cells, wind, a rush of air when a sealed room is
  opened, pressure evening out across a room in well under a second, blasts that push things.
  It would replace the per-cell current (`aux` wind), the long pressure jumps, the column-window
  buoyancy and probably the breathing stir (`STIR_CELLS`).
- **Deliberate deviations (to document in ELEMENTS.md):** sound slowed to about 10–20 m/s so the
  explicit update is stable at 60 ticks/s (real: 340 m/s); flow is smooth below one block; wind
  faster than about one cell per tick (3 m/s) needs sub-steps or a cap.
- **Cost (estimate):** a 256 × 256 velocity grid for a 1024 × 1024 area is 65 000 blocks, under
  1 ms.
- **Main risk: thin walls.** A one-cell wall crossing a block must still be airtight. The blocks
  need connectivity per tile (the pieces of open cells in a tile, as water pressure already keeps
  per chunk in `pressure.c`), recomputed only where solids change. This is the hardest part and
  should be prototyped alone first.

### 4. Simulate less

- **Gas at 30 Hz** (every other tick, with a doubled time step, alternating tiles), while liquids,
  powders, particles and creatures stay at 60 Hz. Gas is slow and smooth. With today's rules: the
  rates per update doubled (diffusion, buoyant and current moves); a cell still moves at most one
  cell per update, so the fastest rise and currents halve (1.5 m/s).
- **Only near the player** (ROADMAP M6b): the visible part of the level plus a margin, so world
  size stops mattering.

### 5. Smaller gas storage

20 bytes per cell today (5 floats), plus 4 for the cached density: 190 MB at 4096 × 2048.
Nitrogen as "the rest" and the other species as 16-bit fractions of the cell's total moles, plus
the total, is about 12 bytes. Less memory traffic for the field pass, but conversions in every
loop: measure before deciding. Only useful after step 1.

### 6. Free particles

Not a bottleneck today (a bullet costs a few hundred cell checks; the list is serial, up to
131 072 particles). If jets or debris grow to tens of thousands in flight: bin them by chunk and
step the bins in parallel with the same checkerboard as the movement pass (landing writes cells).
With step 3, particles can take drag from the wind (casings, debris and splashes carried by a
blast), which adds credibility rather than speed.

### Tried and dropped

- **Equalising pressure toward the difference instead of in a random direction** (2026-11-24).
  When a cell's pressure differs only from the cell 16 away (a gentle gradient, a third of the
  flagged cells when firing), equalise with that cell rather than at a random direction and
  reach, which misses it most of the time and leaves the cell awake for nothing. Callgrind, 4
  seeds: 11% fewer instructions firing, 15% fewer for the CO2 ball, on every seed; wall clock, 1
  thread, 3 runs each, alternately: 17.7 against 17.2 ms (no gain; the partner 16 rows away is
  probably a cache miss). Aiming at the most different neighbour as well cost 5% more
  instructions. Also tried: a finer far tolerance (0.17%) to keep the two-room residual within
  its test (aiming alone left 3.7% against 3.0%). Dropped: no time gained.
- **Not waking whole chunks for convection** from the heat pass (an experiment, to see whether
  those wakes made the awake region broad): no change in cells visited. They are not the cause.
- Letting cells that hold water vapour take the "exactly like its neighbours" exit too: no change
  in the firing case (28.4 / 29.0 ms against 30.1 / 28.8 ms at 10 s, 1 thread).

### Order

Revised 2026-11-24, after step 2 and the in-game measurement of step 1. What is known: a busy
scene keeps 120 000–200 000 gas cells awake, and today's rules cost about 100–140 ns per visited
cell on this VM (whole tick, one thread); the cases measured here run at 6–11 ms per tick on 4
threads.

1. Step 2 (tiles): done, about 5% (above).
2. Measured on the player's machine (done): a laptop Ryzen 7 4700U (8 cores, 8 threads), the
   default level, firing at the sky for 10 s while sweeping from left to right: 2 million cells
   updated, Sim 50 ms, 20 fps. The same scene here: 2.06 million, 74 ms on 4 threads (so that
   laptop with 8 threads is about 1.5× this VM with 4). The cause was not the gas rules' cost per
   cell but how much was awake: bullets fired upward knock soil out of the dirt ceiling, spent
   bullets fall back, and every cell of debris falling through the sky churned the air along its
   whole path for seconds (1.8 million cells with a current) and, for bullets, heated it (a cell
   of lead at 400 K holds 350 times a real bullet's heat: the heat pass ran on 690 chunks). Fixed
   (Benchmark log): lone falling cells barely kick the air, currents stop in uniform gas, bullets
   and casings start at room temperature. The scene now costs about 8 ms per tick here at 10 s.
3. Gas at 30 Hz by tile parity (step 4): half the tiles' gas cells each tick, rates doubled.
   Estimate: about 1.3× for the whole tick (gas rules are about 55% of it), at some cost in
   fidelity (buoyant rise and currents at most one cell per update).
4. Fewer wakes: a cell that exchanges gas wakes the 3 × 3 cells around both ends, and 60 000
   visits per tick when firing end within tolerance. Waking only the neighbours that now differ
   from the changed cell. Estimate: about 10%.
5. Thread scaling for desktops with 8 or more cores (4 threads gave 2.5×). Measured in the sweep:
   serial work is under 5% of the tick; a movement phase's jobs add up to 2.3 ms, its costliest
   job takes 0.66 ms and the phase 0.90 ms on 4 threads, and an empty batch cost 55 µs to wake the
   workers. Done (Benchmark log): costliest jobs first, workers spinning briefly between batches.
   Left: a phase can never be shorter than its costliest chunk (a quarter to a third of the
   phase's work in busy scenes), which bounds what more cores can give; only smaller jobs would
   help, and they need a smaller `MAX_MOVE`.
6. Steps 1 and 3 (field pass and velocity) only together, only with one array per species, and
   only if the above is not enough: they are a rewrite of the gas model for a gain measured above
   at between 1.4× and none.

## Benchmark log

| Date | Commit | Machine (CPU, cores) | Compiler | World | Threads | ns/updated cell | ms/tick p50 / p99 | Change |
|---|---|---|---|---|---|---|---|---|
| 2026-11-22 | M1 | Cloud VM, Xeon @ 2.1 GHz (4 vCPU) | gcc 13.3 -O3 | 1024×768 bench scene | 1 | 62.2 | 13.6 / 19.3 | First M1 version |
| 2026-11-22 | M1 | same | gcc 13.3 -O3 | 1024×768 bench scene | 1 | 57.8 | 12.5 / 18.7 | Multi-cell moves as one swap; paired dirty-rect wake |
| 2026-11-22 | M1 | same | gcc 13.3 -O3 | 1024×768 bench scene | 1 | 58.3 | 12.7 / 19.1 | Final M1 liquid rules (pressure, lookahead, wander, streams): ~231k updated cells/tick |
| 2026-11-22 | M1 | same | clang 18 -O3 | 1024×768 bench scene | 1 | 58.1 | 14.0 / 17.3 | Same code, Clang |
| 2026-11-22 | M1+ | same | gcc 13.3 -O3 | 1024×768 bench scene | 1 | 59.8 | 13.1 / 19.2 | Surface liquid looks through its own liquid (no measurable change) |
| 2026-11-22 | M1+ | same | gcc 13.3 -O3 | 1024×768 bench scene | 1 | 56.2 | 12.3 / 16.0 | Falling liquid waits behind falling liquid (no measurable change) |
| 2026-11-23 | M1+ | same | gcc 13.3 -O3 | 1024×768 bench scene | 1 | 59.6 | 12.5 / 20.1 | Pressured liquid looks through its own liquid; wait only if the column below has room (within noise) |
| 2026-11-23 | M2 | same | gcc 13.3 -O3 | 1024×768 bench scene | 1 / 2 / 3 / 4 | — | 12.4 / 7.7 / 5.5 / 4.1 (p50) | Parallel phases. 3.0× at 4 threads. Before the stack-copy fix (false sharing on job contexts): 12.2 / 11.9 / 9.7 / 8.0 |
| 2026-11-23 | M2 | same | gcc 13.3 -O3 | 4096×2048, `--fill 0.35` (≈1.0 M active) | 1 / 4 | 67.3 / 18.2 | 67.6 / 19.1 (p50) | 3.7× at 4 threads |
| 2026-11-23 | M2 | same | gcc 13.3 -O3 | 4096×2048, `--fill 0.7` (≈2.3 M active) | 4 | 18.7 | 41.8 / 51.0 | The M2 target scene: about 24 ticks/s on this VM |
| 2026-11-23 | M3 | same | gcc 13.3 -O3 | 1024×768 bench scene / 4096×2048 `--fill 0.35` | 4 | — | 4.42 / 22.9 (p50) | Heat, convection, temperature buoyancy. +8%. Before skipping heat for uniform temperatures (D-023): 7.75 / 29.2 |
| 2026-11-23 | M3 | same | gcc 13.3 -O3 | same, `--temp-spread 30` (heat everywhere) | 4 | — | 9.9 / 52.2 (p50) | Worst case: every chunk conducting and convecting |
| 2026-11-23 | M2b | same | gcc 13.3 -O3 | 1024×768 bench scene / 4096×2048 `--fill 0.35` / `--fill 0.7` | 4 | — | 4.07 / 21.3 / 42.1 (p50) | Water pressure (body labels + union-find + transfers). Before: 4.21 / 20.6 / 41.8. Relabelling only when *still* liquid changes was needed: relabelling on any liquid movement cost +25% on the big scene |
| 2026-11-23 | M4 | same | gcc 13.3 -O3 | 1024×768 bench scene / 4096×2048 `--fill 0.35` | 4 | 28.3 / 27.9 | 10.9 / 52.2 (p50) | Gas mass and pressure. M3 on the same day and machine: 4.15 / 17.0. The scene's gas now fills its vacuum pockets, so ~1.7× more cells are active (392k vs 237k per tick at 1024×768, 51k pressure equalisations per tick), and gas rules cost more per cell (~28 vs ~19 ns). Mixing check first written per neighbour: 13.3 ms; one 13-cell neighbourhood load, cheap sleep checks first, 3×3 wakes at both ends of a transfer: 10.9 |
| 2026-11-23 | M4 | same | gcc 13.3 -O3 | 1024×768 bench scene / 4096×2048 `--fill 0.35` | 4 | — | 13.0 / 58.0 (p50) | Gas pressure on liquids and balance across gases. Before: 10.9 / 52.2. Nearly all from the cross-gas balance (breadth-first region walks in the gas soup): first version, every eligible tick with 256-cell regions, 14.0 / 190 with p99 221 / 926 ms; 64-cell regions at 1/8 chance: 13.5 / 60.9; at 1/16: 13.0 / 58.0. Without the balance: 11.3 |
| 2026-11-23 | M4b | same | gcc 13.3 -O3 | 1024×768 bench scene / 4096×2048 `--fill 0.35` | 4 | — | 14.5 / 61.8 (p50) | Free particles, jets, splashes. Before: 13.0 / 58.0. About 200 particles in flight; the particle update itself is negligible, the cost is splashes leaving holes that wake their neighbours (414k vs 394k updated cells) |
| 2026-11-23 | M4c | same | gcc 13.3 -O3 | 1024×768 bench scene | 4 | — | 15.8 / 16.4 (p50, M4b / M4c builds run alternately) | Creatures; the bench scene has none, so no change beyond noise (this VM measured 15.4–17.2 ms for one build in this session, slower than earlier in the day). A breathing creature keeps the gas around it awake: a sealed 126 × 62 room of air with one person costs about 1.2 ms/tick |
| 2026-11-23 | M5a | same | gcc 13.3 -O3 | 1024×768 bench scene | 4 | — | 16.7 / 16.9 (p50, two runs) | Powders (a new case in the movement switch); the bench scene has none. Within this session's noise (15.4–17.2 ms for one build) |
| 2026-11-23 | M5c | same | gcc 13.3 -O3 | 1024×768 bench scene | 4 | — | 16.8 / 18.3 / 16.1 vs 17.9 / 17.4 / 15.8 (p50, new vs M5b, alternately) | Reactions. First version checked every hydrogen cell's neighbours for ignition: +5–10%. Gated by the chunk temperature ranges (nothing near the ignition temperature: no check): within noise |
| 2026-11-23 | M5d | same | gcc 13.3 -O3 | 1024×768 bench scene | 4 | — | 16.2 / 16.0 (p50, two runs) | Liquids push each other (column scans in labelling, requests and transfers). Within noise |
| 2026-11-23 | M5 currents | same | gcc 13.3 -O3 | 1024×768 bench scene / 4096×2048 `--fill 0.35` | 4 | 40 / 38 | 14.1 / 14.6 and 66.0 (p50) | Gas currents (falling water stirs the gas in the soup): fewer cells updated (364k vs 394k; the mixtures settle sooner), more moved |
| 2026-11-23 | M5 mixtures | same | gcc 13.3 -O3 | 1024×768 bench scene / 4096×2048 `--fill 0.35` | 4 | 56 / 52 | 21.4 / 78.3 vs 13.4 / 55.4 (p50, new vs M5 currents, alternately) | Gas mixtures stay mixed: bulk buoyancy between gases (D-030). +60% / +41% on the gas soup, the worst case: every cell a random one of three very different gases, all awake. Window sums (up to 450 reads; first version tried every tick at all heights: 27.7 ms) and the soup no longer unmixing (more cross-gas balancing, `world_gas_region` +30%). Trying the 4-row windows one tick in 2, spreading one in 8, all heights one in 16; branch-free window sums; the significance test, which stops the soup's noise from moving it. Air (O2/N2) pays nothing; settled mixtures sleep |
| 2026-11-23 | M6a | same | gcc 13.3 -O3 | 1024×768 bench scene | 4 | — | 22.5 / 21.0 vs 22.4 / 21.3 (p50, new vs M5 mixtures, alternately) | Player, rifle, bullets, small clumps, Air brush. No change on the bench scene (no bullets in it; this VM was slower than earlier in the day). The new default level (4096 × 2048, air over dirt) costs about 245 ms for its first tick, then 3.5–5 ms/tick idle with the player breathing; painted air is settled at once (random O2/N2 cells used to keep about 2 M cells reshuffling for minutes). A bullet costs a few hundred cell checks, serial |
| 2026-11-23 | M6a casings | same | gcc 13.3 -O3 | 1024×768 bench scene | 4 | — | 15.6 / 15.8 vs 15.2 / 16.9 (p50, new vs M6a tests, alternately, `--ticks 200`) | Powders in flight pass through bodies, walkers kick small piles, bullets start at the muzzle. No change (no creatures in the bench scene) |
| 2026-11-23 | M6a diffusion | same | gcc 13.3 -O3 | 1024×768 bench scene / default level (4096×2048) idle | 4 | — | 16.1 / 17.7 vs 16.4 / 18.2 (bench p50, alternately); level 4.0 / 5.6 / 7.9 vs 4.5 / 5.3 / 4.2 ms at 1 / 2 / 10 s | Gas diffusion down concentration gradients (D-032). The first version ran the precise gradient test (up to 800 reads) for every awake air cell: idle level 14–31 ms. Gating it with 8 × 8 gas block counts recounted each tick (one read per cell of the awake chunks) brought it back; the remaining cost at 10 s is the player's breath diffusing |
| 2026-11-23 | M6a heaps | same | gcc 13.3 -O3 | 1024×768 bench scene | 4 | — | 18.6 (p50, `--ticks 200`) | Walkers climb heaps instead of kicking their foot; kicked pixels fly farther; bullets are projectiles only above 30 m/s. No change expected or seen beyond this VM's noise (no creatures or bullets in the bench scene) |
| 2026-11-23 | M6a breath, muzzle, metals | same | gcc 13.3 -O3 | 1024×768 bench scene | 4 | — | 16.6 / 17.3 vs 18.3 / 16.4 (p50, new vs M6a heaps, alternately, `--ticks 200`) | Turbulent heat exchange in the heat pass (one more byte read per cell: the gas current), lungs, muzzle gas, lead and brass, powder lifted in mid-air, liquid flowing around bodies. No change beyond this VM's noise |
| 2026-11-23 | M6a gas mixtures (D-035) | same | gcc 13.3 -O3 | 1024×768 bench scene; 4096×2048 fill 0.7; 1024² room: shooting into the air, CO2 ball (r = 100); default level; demo scene | 4 | — | bench 22.0 vs 18.2 (p50); 4096×2048: 203 vs 161; shooting 5.7–6.0 / 7.8–9.1 / 11.4–11.5 vs 7.1–7.9 / 11.2–11.8 / 13.6–13.8 ms at 1 / 5 / 10 s (two runs each, alternately); CO2 ball 4.7–5.2 / 9.1–9.4 / 11.0–12.0 vs 9.4–10.3 / 13.4–15.4 / 17.7–19.8; level: first tick 117 vs 271 ms, then asleep (was 13 ms for 2 s); demo (gas blobs expanding into a world of vacuum) 36 vs 14 ms at 8 s | A gas cell holds a mixture: Fick diffusion with a neighbour, column-window buoyancy on a per-tick density cache, gravity currents, in-cell burning, dew point. The cases that dropped the game to 10 fps are 1.2–2× faster; the bench soup of random single cells of pure gases is 20–25% slower (every cell differs), and gas expanding into a big vacuum 2.5× slower (it fills it faster, as more cells). Found on the way: still cells cost 153 ns (now a few loads when exactly equal, ~40 ns within tolerance); pressure churn from heating (tolerance 0.1% → 0.3%, 0.2% per 16 cells, 0.3% of 0.1 atm absolute); heat pass computes each pair once (shooting −20%). Left: a chunk's bounding-rect wake (a bullet trail wakes whole chunks) and few chunks per checkerboard phase (4 threads give 2.5×) |
| 2026-11-24 | M6c tiles | same | gcc 13.3 -O3 | 1024×768 bench scene; 1024² room: firing, CO2 ball (r = 100); default level, firing | 4 | — | bench 17.1 / 19.1 vs 18.4 / 16.1 (p50, `--ticks 200`, alternately); firing 6.31 / 6.51 vs 6.64 / 6.86, CO2 ball 6.94 / 6.89 vs 7.13 / 6.83, level firing 11.0 / 10.6 vs 10.3 / 11.2 (ms/tick averaged over 10 s, alternately) | Activity per 16 × 16 tile (D-036 step 1) and cached densities in the gas sleep check. About 5% on firing in the room, the rest within noise; 8–18% fewer cells visited. Callgrind, firing, 1 thread: 43.1 G → 40.9 G instructions over the run. Estimated up to 2×: the awake region is broad stirred air, not thin trails |
| 2026-11-24 | M6c clear air | same | gcc 13.3 -O3 | default level, firing at the sky for 10 s sweeping left to right (the player's test); 1024² room, firing; 1024×768 bench scene | 4 | — | sweep 7.6 / 6.7 vs 42.8 / 40.3 (ms/tick averaged over 10 s), at 10 s 8.5 / 7.7 vs 77.9 / 77.3, cells visited 156 000 vs 2 060 000, heat chunks 56 vs 692; room 6.2 / 6.9 vs 6.8 / 7.7; bench 19.8 / 19.8 vs 20.0 / 19.2 (p50) (all alternately) | Lone falling cells kick only the gas they displace (`FALL_WIND_LONE`), currents stop in gas exactly like its neighbours, bullets and casings fired at room temperature. Each part measured in the sweep: bullets at room temperature alone 2.0 M cells visited (the currents remain), with currents stopping in uniform gas 209 000, with lone cells as well 156 000 |
| 2026-11-24 | M6c threads | same | gcc 13.3 -O3 | default level sweep (as above); 1024² room: firing, CO2 ball; 1024×768 bench scene | 4 | — | sweep 5.55 / 4.49 / 5.13 vs 5.97 / 7.05 / 5.83 (ms/tick averaged over 10 s; costliest-first alone 5.48 / 5.69 / 5.30); CO2 ball 5.61 / 5.56 vs 6.85 / 6.66; room firing 4.52 / 3.86 vs 4.56 / 4.73; bench 16.1 / 16.4 vs 18.1 / 18.5 (p50) (all alternately) | Movement-phase jobs run costliest first (`Chunk.cost`: cells visited last time); pool workers spin up to 0.2 ms between batches before sleeping (an empty batch: 55 → 12 µs). About 15–20% on 4 threads; more expected with 8, where the costliest job bounds each phase |

Notes on the M1 baseline:
- The bench scene is a worst case: 65% of the box is a random mix of water, three gases and
  vacuum, so almost every cell moves every tick.
- This VM is slow: a trivial dependent-ALU loop runs about 2–3× slower than on a current desktop
  CPU. Expect roughly 20–30 ns/cell on a desktop.
- Callgrind attributes most of the time to swaps and dirty-rect bookkeeping. What remains is
  dominated by unpredictable branches (random walk directions), which is inherent to the
  automaton. The big lever is multithreading (M2).
- In the demo scene, the in-game HUD shows 2–5 ms/tick for 30–50k updated cells (M1–M3). From
  M4 the demo's gas blobs expand into the whole vacuum, and the HUD shows about 10 ms/tick for
  300k updated cells while they spread.
