# Roadmap

Each milestone ends with something runnable. Tick items as they land, and keep the acceptance
criteria honest: a milestone is done only when all of its criteria pass.

## M0 – Documentation ✅

- [x] CLAUDE.md, README, vision, architecture, simulation, elements, performance, code style and
      decisions docs

## M1 – Sandbox box

**Goal:** a box with solid dirt walls, filled with void. A toolbar lets the user paint gases, walls
and water, and the simulation makes them behave plausibly.

Build and skeleton
- [ ] CMake project; SDL3 fetched via FetchContent at a pinned tag; targets `pixel-roguelike`,
      `pr_sim` (static library), `pr-tests`, `pr-bench`
- [ ] Options `PR_SANITIZE`, `PR_NATIVE`, `PR_USE_SYSTEM_SDL`; warnings enabled (see CODE_STYLE)
- [ ] `.github/workflows` CI: build and test on Linux (GCC and Clang) and Windows (MSVC)
- [ ] Main loop with a fixed 60 Hz timestep, decoupled rendering, and a cap on ticks per frame

World and simulation (single-threaded in M1, but already chunk-structured)
- [ ] World grid, default **1024 × 768** (each side a multiple of 64), in 64 × 64 chunks
- [ ] Initial scene: everything void, with a **dirt box** whose walls are 16 px thick around the
      edge. Cells beyond the world edge read as an immovable wall
- [ ] Element table with the M1 elements: void, dirt, rock, iron, ice (static solids), water
      (liquid), oxygen, CO2, hydrogen (gases). Properties come from `ELEMENTS.md`
- [ ] Temperature buffer exists and is initialised (293.15 K). Heat does not flow yet; the value is
      shown in the hover inspector
- [ ] Movement rules: static solids never move; liquids fall, flow diagonally, and spread sideways;
      gases drift and diffuse; density-based swaps (water sinks through gas, CO2 sinks below O2,
      H2 rises)
- [ ] Clock bit, so a cell is never updated twice in one tick
- [ ] Bottom-up scan with the horizontal direction alternating each tick (no left/right bias)
- [ ] Per-chunk dirty rects, so settled regions are skipped

Rendering and UI
- [ ] Streaming texture at world size; base colour + `shade` variation per element; gases drawn as
      translucent tints
- [ ] The world is scaled to fit the window with its aspect ratio kept (letterboxed), and pixels
      stay crisp (nearest-neighbour)
- [ ] Toolbar with one button per paintable element, plus Eraser (paints void). Keys `1`–`9`
      select tools
- [ ] Brush: circular, radius 0–256 px (0 = a single pixel). Mouse wheel or `[` / `]` resizes it.
      LMB paints the selected element, RMB erases. Strokes are interpolated between mouse samples
      so there are no gaps. The brush outline is shown at the cursor
- [ ] Optional for M1: a square brush shape (toggle with `B`)
- [ ] HUD: FPS, simulation ms/tick, brush radius, selected element, and a hover inspector showing
      the element and temperature (°C) of the cell under the cursor
- [ ] Time controls: `Space` pauses or resumes, `.` advances one tick while paused

Tests and benchmark
- [ ] Behaviour tests: solids never move; water falls and levels out; gas fills upward;
      CO2 ends up below O2; H2 ends up above O2; water sinks through gas; per-element cell counts
      are conserved over 1000 ticks; identical seed + inputs give an identical world
- [ ] `pr-bench`: fills a world with a churning mix, runs N ticks, and reports ns per updated cell
      and ms/tick at p50 and p99. Record the baseline in PERFORMANCE.md

**Acceptance**
- Launching shows the dirt box. Every toolbar element can be painted at any brush size, and
  erasing works, including through the box walls.
- Water poured into the box falls, splashes sideways and settles to a flat surface. Painted CO2
  pools at the bottom, H2 collects at the top, and O2 sits in between.
- Steady 60 FPS on the default world while painting continuously on a mid-range desktop
  (Release build).
- All tests pass under ASan/UBSan.

## M2 – Performance foundation

- [ ] Thread pool (SDL threads), one worker per core minus one
- [ ] 4-phase checkerboard chunk scheduling; max displacement of 32 px per tick enforced (assert in
      Debug)
- [ ] Dirty rects propagate across chunk borders; chunks sleep and wake correctly
- [ ] Deterministic per-chunk RNG; test that 1 thread and N threads produce the same world
- [ ] Texture upload of dirty chunks only
- [ ] Scoped profiler timers shown in the HUD (sim / heat / upload / render)
- [ ] Default world grows to **4096 × 2048** (8.4 M cells); camera pan (MMB drag / WASD) and zoom
      (Ctrl+wheel)

**Acceptance:** 4096 × 2048 world, 60 ticks/s with about 2 M cells active, on a 6–8-core
desktop. Thread-count determinism test passes.

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
