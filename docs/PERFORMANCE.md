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

  `--scene lava` runs the default level instead (4096 × 2048, simulated around the player as in
  the game) with a film of lava 300 cells long and 2 deep on the floor beside the player, after a
  warm-up of 300 ticks (5 s) so the air over it convects: the case of D-051 and D-052.
- **`perf`** (Linux). In the cloud VM used for development there are no hardware counters (no
  cycles, cache misses or branch misses), but timer sampling works and shows where the time goes
  line by line (not why): `apt-get install linux-tools-generic`, then the binary under
  `/usr/lib/linux-tools-*/perf` (the `perf` wrapper refuses a kernel it does not know):
  `perf record -e cpu-clock -F 2000 -g ./prog` and `perf report --no-children --sort srcline`.
  Build with debug info (RelWithDebInfo, `build/prof`). On a desktop, `perf stat -e
  cycles,instructions,cache-misses` adds the counters this VM lacks.
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
- **Prototyped (2026-11-24, `bench/proto_air_velocity.c`), per cell rather than per block:** a
  velocity on every face between two cells (8 bytes per cell), held at 0 next to walls, which
  makes thin walls airtight by construction (no connectivity needed). Each substep, pressure
  differences accelerate the face velocities (sound slowed to `SOUND` cells per tick, lightly
  damped), then gas crosses every face from its upwind cell with its mixture and heat, at most
  `FLOW_MAX` (0.1) of a cell per face. Measured, one thread, the game's 20-byte mixture per cell:
  stable for sound / substeps ≤ 0.5; sealed one-cell walls, straight or diagonal, let nothing
  through in 2000 ticks; a floor patch held at 1000 K for 20 s leaves the room's pressure even
  (1.0025 atm over the heat and far away: the lava case's 135 kPa against 101 does not happen);
  two rooms at 2 and 1 atm through a 12-cell gap even out to 1% in 8.4 s at sound 2 cells per
  tick with 4 substeps (today: still 3% apart after 50 s); 16–17 ns per cell per substep, so
  67 ns per cell per tick at 4 substeps, over the cells it has to compute. Found on the way: a
  per-face limit of 0.2 let a cell lose 80% of its gas in a substep and a checkerboard locked in;
  velocities must be held within what a face can carry (a gap chokes), or gas piles up.
- **Integrated in the game, then reverted (2026-11-24).** The prototype's scheme as an air pass
  (`src/sim/air.c`, run after the heat pass, before the movement pass): face velocities in
  `World`, computed on the tiles where the air moved or the pressure was uneven, plus the awake
  tiles and heat-active chunks, and a ring of one tile; the four flux classes in place (no buffer);
  `gas_equalize()` removed from the gas rule. It worked as physics: 89 of 90 tests passed
  unchanged, thin walls stayed airtight, the overpressure over the lava pools fell from 135 to
  106–114 kPa. Two findings:
  - A suddenly heated bubble (36% above the room's pressure) rang across its edge, and every
    swing moved hot gas into cold cells and back: it smeared out and stopped rising. Damping
    strong enough to stop that also turns large-scale waves back into diffusion. Artificial
    viscosity (each face velocity relaxing toward its neighbours, in two checkerboard passes)
    damped the ringing and left the bubble rising as before.
  - **Cost.** The lava scene at 30 s: 31 ms per tick without the air pass, 64–144 ms with it on
    4 threads, depending on the thresholds. Steady heating drives a weak outflow over a wide area,
    and with the damping a settling scheme needs, pressure still spreads diffusively at large scale
    (about 100 cells² per tick), so a wide region keeps moving: 2 000–7 000 tiles (0.5–1.8 million
    cells) computed per tick, at about 290 ns per cell per tick of single-thread work (four
    substeps of eight passes, and a full mixture-and-heat move for every face carrying even a
    ten-thousandth of a cell). Raising the sleep velocity cut the tiles but not enough; skipping
    slow faces left 0.1% differences unresolved and those tiles never slept. With damping, a
    pressure difference Δ between neighbours drives a steady flow of about 100 Δ cells per tick,
    so the sleep velocity, the skipped flow and the pressure tolerance must agree, and no setting
    found was both cheap and accurate.
  What it would take: far fewer cells carrying velocity (blocks, which brings back the thin-wall
  connectivity problem), or a pressure solve that settles in one step over a region instead of
  waves (a multigrid relaxation per connected gas region), or simulating only near the player.

### 4. Simulate less

- **Gas at 30 Hz** (every other tick, with a doubled time step, alternating tiles), while liquids,
  powders, particles and creatures stay at 60 Hz. Gas is slow and smooth. With today's rules: the
  rates per update doubled (diffusion, buoyant and current moves); a cell still moves at most one
  cell per update, so the fastest rise and currents halve (1.5 m/s).
- **Only near the player** (done, D-040): the 3 × 3 regions of 512 cells around the player, so
  world size stops mattering.

### 5. Smaller gas storage

24 bytes per cell today (6 floats), plus 4 for the cached density: 190 MB at 4096 × 2048.
Nitrogen as "the rest" and the other species as 16-bit fractions of the cell's total moles, plus
the total, is about 12 bytes. Less memory traffic for the field pass, but conversions in every
loop: measure before deciding. Only useful after step 1.

### 6. Free particles

Not a bottleneck today (a bullet costs a few hundred cell checks; the list is serial, up to
131 072 particles). If jets or debris grow to tens of thousands in flight: bin them by chunk and
step the bins in parallel with the same checkerboard as the movement pass (landing writes cells).
With step 3, particles can take drag from the wind (casings, debris and splashes carried by a
blast), which adds credibility rather than speed.

### Lava: what to try next (2026-11-25, D-051)

From the lava-film measurement (Order, 3). Each with what it does to the physics.

1. **What keeps the lava's room awake (measured 2026-11-25).** The gas cells woken for the next
   tick, classified with the gas rule's own tests in its order (a copy outside the simulation:
   approximate), with the lava film after 40 s against the same level without lava after 20 s:

   | | lava | no lava |
   |---|---|---|
   | awake gas cells | 145 000 | 59 000 |
   | pressure differs from a neighbour (79% of them over 25 °C: the plume) | 29 000 | 4 000 |
   | pressure differs from the cell 16 away (mostly cool air: the front) | 29 000 | 4 000 |
   | woken, but within tolerance (nothing to do: a neighbour changed) | 59 000 | 37 000 |
   | current | 25 000 | 11 000 |
   | density (warm or cold air) | 2 000 | 1 400 |
   | composition, water vapour, void, liquid | ~0 | ~0 |

   So lava adds about 50 000 cells awake for pressure (40%), half of them out in cool air the
   front has reached: idea 2. Warm air rising (density) keeps almost nothing awake. And with or
   without lava the largest group (41–63%) is cells woken around a change that find nothing to
   do: Order, 6 ("Fewer wakes") is worth more than its 10% estimate suggested (such a visit costs
   less than a full update, but it runs the costly checks of `gas_is_mobile`).
2. **Let heated gas expand at once: measured 2026-11-25, dropped as a conserving rule.** The heat
   pass raises a gas cell's pressure and that spreads by diffusion; real air expands at the speed
   of sound. Tried in `apply_chunk` (heat.c): a gas cell whose temperature changes from T_old to
   T_new keeps n·T_old/T_new of its gas, and gives up the rest (taking it in when it cools). Lava
   film, 4 threads' scene on one thread, ms per tick, 40 s:

   | | ms/tick | cells visited | awake gas after 40 s | awake for pressure |
   |---|---|---|---|---|
   | today | 31–34 | 150 000 | 145 000 | 58 000 |
   | the excess **discarded** (not conserving: the ceiling of the idea) | 24–25 | 128 000 | 99 000 | 9 500 |
   | the excess **pooled per chunk and shared out** over the gas of the 3 × 3 chunks around it, with its heat (conserving; a third parallel pass in which each job pulls the pools around it) | 49–52 | 224 000 | 280 000 | 82 000 |

   So the idea is worth at most about 20%, and only if the gas that leaves is *not kept*. Kept, it
   lands as an overpressure on the cells that receive it, in blocks the size of the window, and
   those differ from their neighbours and from the cells 16 away, so more cells stay awake than
   before, and the pass itself costs more (a lava room's 100 chunks × 4 096 cells). The physics
   explains it: in the world the excess goes to a huge connected volume (a sealed room does rise in
   pressure; an open one does not), and a rule that is right for both must know how much volume is
   connected. Discarding it is right only for gas that is open to the world; a sealed tank heated
   (a pressurised vessel, gas pushing on water, D-025) must keep it. Telling them apart needs the
   connected region of every heated cell (a flood fill of up to tens of thousands of cells per
   chunk per tick), which costs about what it saves. With the discard, warm air rises as it
   should (39% of the awake cells wait on density, not pressure). Same conclusion as the two
   earlier attempts at faster pressure (relaying, step 3). Left: a cheap openness test, for
   example by chunk (does the chunk's gas touch the border of a simulated region that is not
   closed?); not built.
2b. **Fewer wakes: measured 2026-11-25, dropped.** `gas_wake` after an exchange wakes the 3 × 3
   cells around both ends. Waking only the two cells themselves (more extreme than any exact rule,
   and it breaks behaviour) cut cells visited by 4% (143 758 against 150 211) and the time not at
   all (30.0 against 29.8 ms, lava film, one thread). The cells woken "for nothing" (41–63%) are
   in tiles that some other cell keeps awake (visits are by tile and rect, and cells keep
   themselves awake), so waking fewer cells does not shrink the set. Cell-level activity would
   need a per-cell flag or list: a different design, as in step 1.
3. **Fewer instructions per awake gas cell** (tried 2026-11-25: cached moles, no gain, below), with
   the same rules:
   - work out each awake cell's moles and moles × temperature once per tick, in the pass that
     already works out its density (`gas_rho_update`), instead of again for itself, each
     neighbour and the cells 16 away. Decisions would see values up to a tick old, as with the
     density already; transfers still use the live gas, so mass and energy stay exact. Same
     behaviour on average, not cell for cell. Costs 4–8 bytes a cell (32–64 MB at 4096 × 2048),
     unless kept only for simulated chunks;
   - the buoyancy scan computing the same result faster (running sums, stopping once decided);
     scanning fewer scales instead would change how plumes rise: not that;
   - skip work only where the answer is certain to be "nothing": many awake cells wait on a
     random roll (warm air rises with a probability each tick), and skipping those would change
     rates.
   Not the "one random direction per tick" rule: changing it changes diffusion and pressure
   rates, which are tuned to real constants (the mixing tests) and would need retuning.
4. **The heat pass less often where only solids warm (measured: nothing to slow).** In the lava
   scene all 88 heat chunks hold gas (rooms), none is solid-only; the pass costs 2.0 of 9.8 ms on 4
   threads (7.5 of 33 on one: compute 5.8, apply 1.7). Original idea: About a quarter of the lava tick (2.4 ms
   on 100 chunks), much of it dirt slowly warming around the lava (to be counted). Heat moves so
   slowly in solids that a step every 4th tick, 4 times as long, stays stable (explicit conduction)
   and looks the same. Physics: the same diffusion, coarser in time.
5. **Radiation from hot surfaces (done, D-052).** Real lava at 1 200 °C loses most of its heat by radiation
   (about 120 kW/m²) to the walls, not the air, and crusts quickly; here all of it goes into the
   air by conduction (sped up ten times for gas). Physics: more realistic, but lava looks different
   (it crusts and darkens sooner). A feature, not a tweak.
6. **Gas off the screen at a lower rate** (not tried; the 30 Hz experiment suggests at most 10–15%) (every 2nd or 4th tick) inside the simulated area
   (1 536 × 1 536 cells, larger than the screen). Physics: off-screen gas evolves more slowly.
   Little help for lava beside the player, which is on screen.
7. **The phase floor** (not a fix): a phase is never shorter than its costliest chunk, about 1 ms
   for a chunk of awake gas, so a lava room puts about 4 ms under the tick whatever the thread
   count; an 8-thread laptop is closer to it than this 4-thread VM. Ideas 2, 5 and 6 lower it.

**Where the time goes, sampled with `perf` (2026-11-25).** Lava film, one thread, 97 000 samples
(timer sampling: a stall is charged to the instruction that waited, give or take a few):
the movement pass 48% of the tick (the gas rule inlined in `phase_job`), `gas_buoyancy` 10%,
`gas_equalize` 9%, the heat pass 20% (`compute_chunk` 14%, `pair_flux` 3%, `apply_chunk` 3%),
`gas_rho_job` 5%. By line, the time sits on the *first reads of other cells' gas*: summing a
neighbour's or a far cell's moles (`gas_moles`, 14%), comparing two pressures (`gas_differs`, 8%),
comparing two mixtures (6%), the column scans of the buoyancy test (4%), `world_elem` (4%), and in
the heat pass building the halo (`gas_heat_cap`, 5%). (A first reading, "the cost is reaching
other cells' data, not arithmetic", was overturned by the counters below.)

**Hardware counters on the player's laptop (2026-11-25).** Ryzen 7 4700U (Zen 2, 8 cores, about
4 GHz while busy), gcc 13.3, one thread, `perf stat -e cycles,instructions,cache-misses,
L1-dcache-load-misses` (on Zen 2 `cache-misses` counts L2 misses). The cloud VM used so far has no
hardware counters.

| | bench scene (`pr-bench --threads 1 --ticks 200`) | lava (`pr-bench --scene lava --threads 1 --ticks 300`) |
|---|---|---|
| ms/tick (p50), ns per updated cell | 83.4, 223 | 34.8, 229 |
| updated cells/tick | 380 252 (200 621 moved) | 152 888 (5 824 moved) |
| instructions per cycle | 1.57 | **2.28** |
| per updated cell: cycles / instructions | ~920 / ~1 450 | ~915 / ~2 100 |
| per updated cell: L1 data misses / L2 misses | ~42 / ~3.4 | ~46 / ~4.2 |

(Per-cell figures divide the whole run, warm-up and level setup included, by the measured ticks:
approximate.) What it means:
- **Lava is mostly instruction count.** At 2.28 instructions per cycle the core is busy; the four L2
  misses per cell (about 40 cycles each from L3) are worth roughly 15–20% of the time, and the L1
  misses that hit L2 are mostly hidden behind the work. About 2 100 instructions per awake gas cell
  per tick is the cost to cut.
- **The bench scene is more memory-bound** (1.57 per cycle): water and gas moving every tick touch
  far more cache lines per useful instruction.
- **The cloud VM misjudges instruction cuts.** Two experiments that removed instructions (cached
  moles, density before composition, Tried and dropped) showed no gain on the VM; on this CPU they
  should. The VM even gives the same ns per cell as the 4 GHz laptop. Judge hot-path changes on a
  desktop CPU (CLAUDE.md → Workflow).

**Where the L1 misses happen, lava** (`perf record -e L1-dcache-load-misses,cache-misses`, then
`perf report --no-children --sort srcline`, 127 000 samples):

| share of misses | code | what it reads |
|---|---|---|
| ~33% | `gas_differs` (sim.c, `nn * w->temp[ni]`), `gas_moles` (world.h), `gas_mix_differs` | the 4 neighbours' gas and temperature (`gas_is_mobile`) |
| ~16% | `gas_buoyancy`, its column loop | up to 16 rows above and below, at 4 scales |
| ~5% | `gas_differs_at` | the cells 16 to the right and below |
| ~4% | `world_in_bounds` / `world_elem` | the neighbours' elements |
| ~4% | `gas_same_as_around` | the "exactly like its neighbours" exit |

**Results on the desktop (2026-11-25).** Candidate 1 (cached moles): no gain, the misses just
move (Tried and dropped). Candidate 2, done differently: not column sums but a per-tile range of
density that lets a column that cannot show a contrast skip the scan (Benchmark log: −12 to −18%
of the cycles, −8 to −12% of the time on one thread, identical behaviour). Candidates 3 and 4
remain. The cycle profile taken before the skip (`perf record -e cycles`, 1 thread): `phase_job`
49%, `gas_buoyancy` 21%, `gas_equalize` 10%, `compute_chunk` 7%; by line `gas_differs` (the
pressure test of `gas_is_mobile`) 10.5% and `gas_moles` 11.5% (not yet re-measured). At 8 threads lava
is 10–13 ms of the 16.7 ms tick (the phase floor, Lava: what to try next, 7).

**Why more threads stop helping (measured 2026-11-25, same laptop, lava film).** 1 / 2 / 3 / 4 /
5 / 6 / 8 threads: 30.8 / 18.8 / 13.4 / 11.0 / 10.6 / 10.6 / 13.3 ms (p50). Clocks hold up (3.74
GHz on one thread, 3.29 on four) but at 8 threads only 3.9 CPUs are busy. Per movement phase
(about 23 jobs of awake chunks, 4 phases a tick), averaged over 400 ticks:

| threads | phase wall | sum of the jobs | costliest job |
|---|---|---|---|
| 1 | 6.28 ms | 6.24 | 1.00 |
| 4 | 1.86 | 6.89 | 1.13 |
| 8 | 1.94 | 7.81 | 1.51 |

The costliest job is about 4× the average (it is the chunk in the plume over the lava, 8–11% of
a phase's cells but the most work per cell), and a phase can never be shorter than it: 1.0–1.5 ms
a phase, 4–6 ms a tick, which is where 4 to 8 threads meet. The other serial or per-chunk
stages at 6 threads: heat 2.0 ms (78 small jobs: 4.6 ms on one thread), density 0.8 (1.7),
pressure 0.2–0.3, the rest under 0.2. Finer jobs need a smaller `MAX_MOVE`, so a smaller chunk or
block (liquids fall up to `MAX_MOVE` rows a tick, element spreads are asserted to be at most
`MAX_MOVE`, the tile mask assumes 4 × 4 tiles): a retuning of the physics, not a refactor.

**Next, on a desktop: candidates to measure, one at a time** (same rules; each judged by
`pr-bench --scene lava --threads 1`, `--scene fire` and the bench scene, old and new alternately, two runs each,
plus the behaviour and thread-count tests):
1. **Cache each cell's moles per tick** (`World.mol`, filled with `World.rho` in `gas_rho_update`),
   read by `gas_is_mobile` and the cheap test of `gas_equalize` instead of summing five species for
   every neighbour and far cell. Decisions see values up to a tick old (as the density already);
   transfers read the live gas. +4 bytes a cell. Tried on the VM only (no change there).
2. **Column sums of density once per tick**, so the buoyancy scan reads two values per scale
   instead of up to 16 cells. Exact only if `World.rho` does not change during the movement pass
   (check how swaps treat it first); otherwise decisions a tick old, as above.
3. **A cheaper mixture comparison**: an exact-equality fast path, or testing density first (the
   same answer).
4. **Fewer instructions in `gas_is_mobile`** generally: it runs for every awake cell that is not
   exactly like its neighbours (most of them) and is the biggest single block.
Then, if lava is still too slow: the far checks less often, or per-tile summaries for them.

Plus Order, 5 (gas at 30 Hz by tile parity), which changes the physics (rise and currents at most
one cell per update). Each change: the behaviour tests, the thread-count tests, the lava film
with `cells_visited` (deterministic: it tells a pure speed-up from a change of behaviour), plume
height, the overpressure over the lava and the CO2-ball mixing time, logged below.

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
- **Instant expansion of heated gas** and **fewer wakes** (2026-11-25): see Lava: what to try
  next, 2 and 2b. The first is worth at most 20% and only without conserving the gas; the second
  gains nothing.
- **Caching each cell's moles per tick** (2026-11-25, `World.mol` next to `World.rho`, read by the
  mobility checks instead of summing the five species for every neighbour): lava film, 1 thread,
  33.5 / 33.9 against 33.4 / 33.5 ms; cells visited 152 642 against 150 211 (stale values
  change a few decisions). No gain **on the cloud VM**; worth trying again on a desktop CPU,
  where the counters show lava bound by instruction count (above).
- **Caching each cell's moles, again, on a desktop CPU** (2026-11-25, Ryzen 7 4700U, `perf stat`,
  lava film, 1 thread, old and new alternately): instructions 234.6 G → 230.4 G (−1.8%), but L1
  data misses 5.25 G → 6.7–6.8 G (+27%: the cached array is one more cache line per neighbour),
  cycles 124 G / 122 G against 124 G / 126 G, ms/tick 35.7 against 35.6. **No gain**: the
  instructions saved are paid for in misses. (The patch is not kept.)
- **A balanced sum in `gas_moles`** (two dependency chains instead of five dependent adds; the
  sleep checks wait on it): same cells visited, 34.9 / 34.3 / 34.4 against 36.7 / 35.1 / 34.0 ms.
  No gain: the latency of the sum is not what the cores wait for.
- **Row stride not a power of two** (lava film at 4112 and 4160 cells wide against 4096, to test
  L1 set conflicts: every array's row stride is a multiple of 4 KB, so the cells of a column map
  to the same few L1 sets): L1 misses fall **4×** (5.2 G → 1.3 G) and the time does not move
  (3% worse per cell: the world differs a little). Conflict misses hit L2 and are hidden, which
  confirms that lava is bound by instruction count and not by the L1. No row padding.
- **`-march=native`** (`PR_NATIVE`, 2026-11-25, Ryzen 7 4700U, lava film, 1 thread, three runs
  alternately): 31.0 / 30.9 / 30.6 ms against 32.2 / 31.0 / 31.5 ms: 0–3%. Not worth a build
  target for the release binary.
- **2 MB pages** for the big arrays (`madvise(MADV_HUGEPAGE)`, 2 MB alignment; the kernel did use
  them, 334 MB resident), on the idea that rows 80 KB apart miss the TLB: lava film, 1 thread,
  39.2 / 39.6 against 33.2 / 32.3 ms: **slower**. Not the bottleneck.
- **Gas at 30 Hz** (skipping the gas cells of half the tiles each tick, tiles in a checkerboard of
  tick parity, the skipped ones kept awake; no rate compensation, so only an upper bound): lava
  film, 1 thread, 34.8 / 32.2 ms before against 32.0 / 27.1 after, cells visited 150 211 → 113 223.
  About 10–15% for a change in how fast air rises and currents move: not done then. (Done since by
  chunk, with the rates compensated, for 1.4–1.5×: D-054.)
- **Comparing density before composition** in `gas_is_mobile` (2026-11-25; the same answer, the
  costly mixture comparison skipped when density already decides): lava film, 1 thread,
  34.1 / 34.2 against 34.2 ms. No gain on the cloud VM; to retry on a desktop.
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
3. **Continuous heat sources (found 2026-11-24, not fixed).** Three small lava pools in the
   default level: 14 ms per tick after 10 s, 74 ms after 2 minutes on 4 threads here, still
   growing (the player saw the frame rate fall to 30 and keep falling). The awake area grows
   steadily (1.4 million gas cells after 2 minutes). Measured causes, in order:
   - **Pressure.** The air over a pool reaches 135 kPa while the sky stays at 101 kPa: heating
     raises a cell's pressure, and pressure here spreads by diffusion (long equalisation jumps
     are allowed only between cells within 2 K, `GAS_JUMP_MAX_DT`, so over hot gas it moves one
     cell at a time). Real air expands at the speed of sound and stays within a few pascals.
     The spreading pressure front keeps everything it reaches awake (most flagged cells are
     flagged for pressure: 248 000 per tick), and the overpressure pushes the lava's surface
     around (1 cell of lava is 1.3 kPa): about 110 lava cells moved per tick, which stirs the air
     over the pools (250 000 cells with a current). Without gas pressure on liquids (an
     experiment) the lava is still and the cost about 10% lower.
   - **Tried:** relaying equalisation along the line (every cell passes the same moles on, so
     heat and composition move one cell while pressure jumps 16) with no temperature limit: the
     overpressure fell to 110 kPa, but the cost rose to 93 ms at 60 s (against 55), as pressure
     kept a wider area busy. Dropped.
   - **Buoyancy** (D-039) made warm air rise as a body, but over the pools it still rises as
     columns topped by single warm cells, which cool within ticks (gas conduction is sped up
     ten times) before they get far: a plume does not reach the ceiling in 2 minutes.
   The fix this pointed to was step 3 (a velocity with a set speed of sound). Prototyped, then
   integrated and reverted the same day: it reduced the overpressure but made this scene 2–4×
   slower (step 3 above). Still open.
4. **Simulate only near the player** (done, D-040): lava pools far from the player 65.7 → 2.5 ms
   per tick at 60 s; near it 59.8 → 53.6 ms (they are inside the simulated area and still grow
   there, more slowly; the pressure front stops at its edge); the sweep test 5.5 → 4.9 ms.
   **Measured again (2026-11-25, D-051)** with a lava film 300 cells long beside the player in the
   level: 4.5 ms per tick before, 11–13 ms after 40 s (4 threads), unchanged by D-051. About
   150 000 gas cells stay awake (the lava's room and the one above), at about 230 ns each on one
   thread. Callgrind: about 450 M instructions per tick on one thread, against about 45 ms
   natively (10 G a second, 3–5 per clock at this VM's 2.1 GHz base), so about 2 000 instructions
   per awake cell: the instruction count looks like the main cost, in the gas rule's checks
   (recomputing a cell's moles, about 16% of all instructions; comparing mixtures, about 14%; the
   multi-scale buoyancy scan, about 10%). But a change that cut 11% of the instructions gained no
   time (Tried and dropped: the partner 16 rows away is probably a cache miss), so some accesses
   do miss the cache: every change must be judged by wall time, not by instruction counts. (A
   first reading of these numbers, "memory-bound", was wrong.) The surface of the lava moves for
   about 15 s (100 transfers a tick: the heated air's overpressure), then settles. Also: a phase
   is never shorter than its costliest chunk, and a chunk of 4096 awake gas cells costs about
   1 ms, so four phases put a floor of about 4 ms under such a tick whatever the thread count.
   What to try next: below, "Lava: what to try next".
5. Gas at 30 Hz by tile parity (step 4): half the tiles' gas cells each tick, rates doubled.
   Estimate: about 1.3× for the whole tick (gas rules are about 55% of it), at some cost in
   fidelity (buoyant rise and currents at most one cell per update).
6. Fewer wakes: a cell that exchanges gas wakes the 3 × 3 cells around both ends, and 60 000
   visits per tick when firing end within tolerance. Waking only the neighbours that now differ
   from the changed cell. Estimate: about 10%.
7. Thread scaling for desktops with 8 or more cores (4 threads gave 2.5×). Measured in the sweep:
   serial work is under 5% of the tick; a movement phase's jobs add up to 2.3 ms, its costliest
   job takes 0.66 ms and the phase 0.90 ms on 4 threads, and an empty batch cost 55 µs to wake the
   workers. Done (Benchmark log): costliest jobs first, workers spinning briefly between batches.
   Left: a phase can never be shorter than its costliest chunk (a quarter to a third of the
   phase's work in busy scenes), which bounds what more cores can give; only smaller jobs would
   help, and they need a smaller `MAX_MOVE`.
8. Steps 1 and 3 (field pass and velocity) only together, only with one array per species, and
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
| 2026-11-24 | M6c buoyancy (D-039) | same | gcc 13.3 -O3 | 1024² room: firing, CO2 ball; default level sweep; 1024×768 bench scene; default level with 3 lava pools | 4 | — | room firing 4.73 / 5.11 vs 4.67 / 4.83; CO2 ball 6.80 / 7.46 vs 6.06 / 6.24 (ms/tick averaged over 10 s); sweep 4.82 / 4.88 vs 5.99 / 4.69; bench 19.0 / 18.5 vs 18.8 / 17.9 (p50) (all alternately); lava pools 85 vs 74 ms at 2 min (one run each) | Sinking as well as rising; moves cross whole layers when they end within 32 rows; a stable vertical density difference no longer keeps gas awake. A big CO2 cloud mixes a little more as it sinks (about +15%); the rest within noise. The lava pools' cost is pressure (Proposed next steps → Order, 3) |
| 2026-11-25 | Radiation (D-052) | same | gcc 13.3 -O3 | 1024×768 bench scene (nothing hot); lava film (300 × 2) in the default level | 4 | — | bench p50 19.7 (22.0–23.5 earlier the same day on a busier machine: no hot matter, so radiation never runs); lava film at 120 s 11.7 vs 13.2 ms, at 300 s 13.9 | Chunks with a cell at 600 K or more radiate, serially, one ray per exposed face; about 600 rays a tick for the film |
| 2026-11-25 | Settling fixes (D-051) | same | gcc 13.3 -O3 | default level: 8 corpses buried in a deep blood pool; 8 corpses on the floor; a lava film; 1024×768 bench scene | 4 | — | buried corpses 190–230 ms (130 000 stuck drops) vs 8–15 ms; corpses on the floor 7.3–7.7 vs 7.0–7.4 ms; lava film 11.6 vs 12.8 ms at 40 s (noise); bench p50 22.2 / 23.5 vs 22.0 / 22.9 (alternately): unchanged | Buried particles are dropped after 60 ticks; corpses float on still blood and sleep when wedged; transfers into gas need two cells of head (pools under warm air no longer flicker) |
| 2026-11-24 | M6b simulated area (D-040) | same | gcc 13.3 -O3 | default level: 3 lava pools far from the player / near it; sweep test; 1024×768 bench scene | 4 | — | lava far 2.5 vs 65.7 ms at 60 s; lava near 53.6 vs 59.8; sweep 4.85 / 4.92 vs 5.55 / 5.44 (averages over 10 s, alternately); bench 19.9 / 20.1 vs 19.2 / 20.1 (p50, everything simulated: unchanged) | Only the 3 × 3 regions of 512 cells around the player are simulated; time stops outside |
| 2026-11-27 | A 6th gas species, rock vapour (D-057) | Ryzen 7 4700U (8 cores) | gcc 13.3 -O3 | `--scene lava`, `--scene fire` (warm-up 600, 300 ticks); lava at 1 thread (200 ticks); 1024×768 bench scene | 4; 1 | — | mean, alternately: lava (4 threads, three runs) 5.98 / 5.85 / 5.55 → 5.81 / 6.39 / 5.92 (+4%); fire 3.76 / 3.92 / 4.27 → 4.07 / 4.27 / 4.24 (+5%); lava, 1 thread 13.36 / 13.28 → 13.38 / 15.34 (the second noisy); bench 23.6 / 24.9 → 23.3 / 24.5 | `GasMix` 20 → 24 bytes: every loop over the species does one more. Each further species would cost about as much (so: rare gas slots instead, ROADMAP) |
| 2026-11-25 | Film condensation (D-056) | Ryzen 7 4700U (8 cores) | gcc 13.3 -O3 | `--scene lava`, `--scene fire` (warm-up 600, 300 ticks); 1024×768 bench scene | 4 | — | mean, two runs each, alternately: lava 6.0 / 5.8 vs 6.3 / 6.4; fire 4.4 / 4.1 vs 4.3 / 4.3; bench 22.9 / 24.1 vs 22.8 / 24.9: unchanged | One more test on the pairs of a gas cell holding vapour and matter that is not gas |
| 2026-11-25 | Zoom ×3 area, half-rate gas, open air (D-053 to D-055) | Ryzen 7 4700U (8 cores) | gcc 13.3 -O3 | `--scene lava` and `--scene fire` (the level, 4096 × 2048; warm-up 600, 300 ticks); 1024×768 bench scene | 1 / 4 / 6 / 8 | — | lava (mean, two runs each, alternately) 31.3 / 30.9 → 12.7 / 12.5 at 1 thread, 10.2 / 10.4 → 5.1 / 5.2 at 4, 9.9 / 9.9 → 4.8 / 5.0 at 6, 10.5 / 10.4 → 5.2 / 5.2 at 8; fire 25.9 / 24.9 → 9.4 / 9.3, 8.0 / 8.1 → 3.8 / 3.8, 7.9 / 8.3 → 3.6 / 3.7, 8.5 / 8.6 → 3.8 / 3.9; p99 at 4 threads: lava 14.0 / 12.2 → 6.4 / 6.1, fire 9.6 / 9.9 → 5.1 / 5.2; lava at 120 s, 4 threads 13.6 → 6.5; bench, 4 threads 28.9 / 28.0 → 22.5 / 23.4 | In steps, 4 threads: area 11.1 → 8.0 (lava), 9.1 → 6.1 (fire); half rate 8.7 → 6.1, 6.4 → 4.7; open air 6.1 → 5.7, 4.6 → 3.9 (each pair measured in its own session: the laptop's clock drifts by 10–20% as it heats). The fire scene is new |
| 2026-11-25 | Buoyancy skip by tile density range | Ryzen 7 4700U (8 cores) | gcc 13.3 -O3 | lava film (the level, 4096 × 2048); 1024×768 bench scene | 1 / 4 / 8; 4 | — | lava, 1 thread: 30.8 / 29.7 / 29.9 vs 34.9 / 32.9 / 31.8 ms (p50, alternately, three runs); `perf stat`: 102 G cycles against 116–124 G (−12 to −18%), 221.8 G instructions against 234.6 G, IPC 2.17 against 1.9–2.0; 4 threads 11.0 / 11.2 vs 11.6 / 12.2; 8 threads 12.3 / 13.5 vs 12.9 / 12.6 (noise: the phase floor); bench 28.9 / 29.2 vs 29.1 / 29.3 (unchanged) | The same cells visited (152 888 per tick, 5 824 moved: identical behaviour). A gas cell whose column of three tiles holds densities within 2 × `GAS_BUOY_TOL` skips the 4-scale buoyancy scan, which cannot find a contrast there: 27 M of the 37.8 M scans in 600 ticks, all of them ones that returned zero (76% of calls did). The range is kept per tile (`World.rho_lo/hi`, computed in the density pass, widened by `swap_cells`, `gas_spawn` and the water-pressure transfers). Checked by running every skipped scan and asserting it finds nothing: zero misses over all 137 tests and the lava scene |
| 2026-11-24 | M6b burning wood (D-044) | same | gcc 13.3 -O3 | 1024×768 bench scene (no wood) | 4 | — | p50 21.3 / 19.9 / 22.8 vs 21.0 (new, new, new vs old; run-to-run noise ±10%): unchanged | One more test on the static cells the movement loop skips; a burning log costs only where it burns |
| 2026-11-24 | M6b rigid objects (D-045) | same | gcc 13.3 -O3 | 1024×768 bench scene (no bodies); the default level (about 55 sleeping bodies) | 4 | — | bench p50 20.4 / 20.7 vs 21.0 (run-to-run noise ±10%): unchanged; level 2.7 ms a tick at tick 600 (was 2.3-2.8) | Bodies are updated serially at the start of the tick: a sleeping one costs a readback and one collision test every 8 ticks; a tumbling log about 0.1-0.3 ms |

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
