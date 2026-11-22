# Roadmap

Each milestone ends with something runnable. Tick items as they land, and keep the acceptance
criteria honest: a milestone is done only when all of its criteria pass.

## M0 – Documentation ✅

- [x] CLAUDE.md, README, vision, architecture, simulation, elements, performance, code style and
      decisions docs

## M1 – Sandbox box ✅

**Goal:** a box with solid dirt walls, filled with void. A toolbar lets the user paint gases, walls
and water, and the simulation makes them behave plausibly.

Build and skeleton
- [x] CMake project; SDL3 fetched via FetchContent at a pinned tag; targets `pixel-roguelike`,
      `pr_sim` (static library), `pr-tests`, `pr-bench`
- [x] Options `PR_SANITIZE`, `PR_NATIVE`, `PR_USE_SYSTEM_SDL`; warnings enabled (see CODE_STYLE)
- [x] `.github/workflows` CI: build and test on Linux (GCC and Clang, ASan/UBSan) and Windows (MSVC).
      Not yet run: the first push will tell. MSVC warnings are not fatal until the code has been
      built there
- [x] Main loop with a fixed 60 Hz timestep, decoupled rendering, and a cap on ticks per frame

World and simulation (single-threaded in M1, but already chunk-structured)
- [x] World grid, default **1024 × 768** (each side a multiple of 64), in 64 × 64 chunks
- [x] Initial scene: everything void, with a **dirt box** whose walls are 16 px thick around the
      edge. Cells beyond the world edge read as an immovable wall
- [x] Element table with the M1 elements: void, dirt, rock, iron, ice (static solids), water
      (liquid), oxygen, CO2, hydrogen (gases). Properties come from `ELEMENTS.md`
- [x] Temperature buffer exists and is initialised (293.15 K). Heat does not flow yet; the value is
      shown in the hover inspector
- [x] Movement rules: static solids never move; liquids fall, flow diagonally, and spread sideways;
      gases drift and diffuse; density-based swaps (water sinks through gas, CO2 sinks below O2,
      H2 rises)
- [x] Tick stamp, so a cell is never updated twice in one tick (D-013)
- [x] Bottom-up scan with the horizontal direction alternating each tick (no left/right bias)
- [x] Per-chunk dirty rects, so settled regions are skipped

Rendering and UI
- [x] Streaming texture at world size; base colour + `shade` variation per element; gases drawn as
      translucent tints
- [x] The world is scaled to fit the window with its aspect ratio kept (letterboxed), and pixels
      stay crisp (nearest-neighbour)
- [x] Toolbar with one button per paintable element, plus Eraser (paints void). Keys `1`–`9`
      select tools
- [x] Brush: circular, radius 0–256 px (0 = a single pixel). Mouse wheel or `[` / `]` resizes it.
      LMB paints the selected element, RMB erases. Strokes are interpolated between mouse samples
      so there are no gaps. The brush outline is shown at the cursor
- [x] Optional for M1: a square brush shape (toggle with `B`)
- [x] HUD: FPS, simulation ms/tick, brush radius, selected element, and a hover inspector showing
      the element and temperature (°C) of the cell under the cursor
- [x] Time controls: `Space` pauses or resumes, `.` advances one tick while paused

Tests and benchmark
- [x] Behaviour tests: solids never move; water falls and levels out; gas fills upward;
      CO2 ends up below O2; H2 ends up above O2; water sinks through gas; per-element cell counts
      are conserved over 1000 ticks; identical seed + inputs give an identical world
- [x] `pr-bench`: fills a world with a churning mix, runs N ticks, and reports ns per updated cell
      and ms/tick at p50 and p99. Record the baseline in PERFORMANCE.md

Also delivered: `--demo`, `--frames`, `--screenshot` options for headless checks; `R` resets
the box; Shift paints only into void; holding the button still keeps pouring.

**Acceptance** (status as of M1 completion)
- Launching shows the dirt box. Every toolbar element can be painted at any brush size, and
  erasing works, including through the box walls.
- Water poured into the box falls, splashes sideways and settles to a flat surface. Painted CO2
  pools at the bottom, H2 collects at the top, and O2 sits in between.
  *Partly met:* this holds wherever gases are in contact (tests cover CO2 below O2 and H2 above
  O2). But gas painted into the empty, vacuum-filled box just spreads out as a thinning cloud
  (particle model, D-005). The box only stratifies once there is enough gas to fill it. M4 (gas
  mass and pressure) fixes this properly.
- Steady 60 FPS on the default world while painting continuously on a mid-range desktop
  (Release build).
- All tests pass under ASan/UBSan.

Verified in development: under Xvfb with scripted mouse and keyboard input (all tools, brush
sizes, square brush, erase through walls, pouring); 15 tests pass under GCC ASan/UBSan and a Clang
`-Werror` build. Not yet verified: 60 FPS on a real desktop GPU and display (the dev container
renders through Xvfb at a steady 59–60 FPS with vsync).

## M2 – Performance foundation

- [ ] Thread pool (SDL threads), one worker per core minus one
- [ ] 4-phase checkerboard chunk scheduling; max displacement of 32 px per tick enforced (assert in
      Debug)
- [ ] Dirty rects propagate across chunk borders; chunks sleep and wake correctly
- [ ] Deterministic per-chunk RNG; test that 1 thread and N threads produce the same world
- [ ] Test that spreading water behaves the same wherever chunk borders fall (D-014). Falling
      streams are covered already (`test_falling_stream_has_no_chunk_bulges`)
- [ ] Experiment: a permanent 1-cell wall ring at the world edge to drop bounds checks from
      neighbour reads
- [ ] Texture upload of dirty chunks only
- [ ] Scoped profiler timers shown in the HUD (sim / heat / upload / render)
- [ ] Default world grows to **4096 × 2048** (8.4 M cells); camera pan (MMB drag / WASD) and zoom
      (Ctrl+wheel)

**Acceptance:** 4096 × 2048 world, 60 ticks/s with about 2 M cells active, on a 6–8-core
desktop. Thread-count determinism test passes.

## M2b – Water pressure (D-018)

Done after M2, because the pressure pass must follow M2's threading rules.

- [ ] Per-cell pressure (hydraulic head) field for liquids, updated by a local relaxation pass
      (reads within 32 cells), only over awake liquid
- [ ] Liquid moves toward lower head: sideways on level-looking surfaces (replaces the random
      wander), and upward when the head exceeds the cell's height (communicating vessels)
- [ ] Settled pools sleep completely: no surface shimmer
- [ ] Pressure overlay view (`P`)
- [ ] Tests: a U-tube balances to within one cell; a poured pool ends level to within one cell
      across the whole basin (no 32-cell slope); a dam holds a taller column than a puddle;
      settled pools sleep
- [ ] Benchmark before/after recorded

## M3 – Temperature

- [ ] Heat conduction between neighbouring cells; energy-conserving; stable (see SIMULATION.md)
- [ ] Moving cells carry their temperature, so convection emerges from movement
- [ ] Hot gas is less dense (ideal-gas scaling), so warm air rises
- [ ] Liquid convection: warm liquid swaps upward through colder liquid of the same kind; water
      is densest at 4 °C. Test: a pool heated from below warms throughout within seconds to
      minutes, not hours
- [ ] Phase changes with latent heat: water ↔ ice, water ↔ steam (new element: steam)
- [ ] Temperature overlay view (`T`), and heat/cool brushes
- [ ] Hot cells glow (blackbody tint)

## M4 – Gas mass and pressure

- [ ] Gas cells carry mass; gas expands into vacuum and thins out; pressure = f(mass, T)
- [ ] Gases flow from high to low pressure; different gases still do not share a cell
- [ ] Pressure overlay view
- [ ] Revisit D-005 and record the outcome

## M5 – Matter variety and reactions

- [ ] Powders (sand, rubble). Digging solids produces rubble or debris
- [ ] Reactions table, e.g. H2 + O2 + ignition → steam + heat (combustion); more as design asks
- [ ] Melting rock or iron into liquids at high temperatures

## M6 – World

- [ ] Save and load (compressed chunks)
- [ ] Procedural world generation
- [ ] World streaming if worlds outgrow memory

## Later (depends on the design answers in VISION.md)

- Digging and building orders: a designation layer painted with the brush, then carried out by
  agents
- Agents (colonists or an avatar), their needs (breathing O2, temperature), pathfinding through a
  pixel world
- Audio, lighting, menus
