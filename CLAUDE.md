# CLAUDE.md

Guidance for Claude Code (and humans) working in this repository. Read this file first, then the
doc that matches your task (see the doc map below).

## What this project is

A 2D, side-view game where **every pixel is simulated**. It draws on *Oxygen Not Included* (gases,
temperature, pressure, a colony underground) and *Noita* (a falling-sand world where everything is
destructible). The working title is the repo name, **pixel-roguelike**. Most of the game design is
still open (see `docs/VISION.md`). The **simulation engine** comes first.

### Non-negotiables

1. **Every pixel is a cell.** Each cell holds exactly one element (void, gas, water, dirt, rock,
   iron, ice, ...) and has its own temperature. Its physical properties (density, heat capacity,
   conductivity, phase) come from that element. A gas cell holds a mixture of the gas species
   (oxygen, nitrogen, CO2, hydrogen, water vapour), its properties from that mixture (D-035).
2. **No tiles.** All player interaction (painting, digging orders, building) goes through a
   pixel-precise brush that can be any size.
3. **Performance is a feature.** The world holds millions of cells. Any change to the simulation's
   hot path is judged by its benchmark result.
4. **Plausibly realistic.** Real-world physical values are the reference. Every deviation is
   deliberate, explicit (a named constant or scale factor), and documented in `docs/ELEMENTS.md` or
   `docs/SIMULATION.md`.
5. **Keyboard and mouse.**

## Current status

**M1 – Sandbox box**, **M2 – Performance foundation**, **M2b – Water pressure**, **M3 –
Temperature**, **M4 – Gas mass and pressure**, **M4b – Flowing water**, **M4c – Creatures** and
**M5 – Matter variety and reactions** and **M6a – Action roguelike: the player and the rifle** are
implemented. The game is an action roguelike: a level of dirt and rock under air in a 4096 × 2048
world, a player (1.85 m) who runs, jumps and fires an automatic rifle whose bullets are real
pixels (they fly, dig, stir the air and pile up with their casings, and melt; each shot blows
hot muzzle gas), a toolbar to paint water,
steam, oxygen, CO2, hydrogen, nitrogen, air, dirt, rock, iron and ice (or erase, dig, heat,
cool), a multithreaded chunked simulation, water pressure,
heat conduction, convection and phase changes, gases with mass and pressure (pushing on
liquids; each gas cell a mixture of gases that diffuse, rise and sink by real density, and
condense; currents), free particles (jets that
follow Torricelli, splashes), creatures that walk, swim,
breathe the air in and out at real rates and give off body heat, powders and digging, lava, molten iron and oil
(liquids push each other), hydrogen combustion, a camera that follows the player (zoom 1), tests
and a benchmark. Next: **M6b – Roguelike, next** (simulating only near the player, HP, heat
damage) and **M6 – World**. See `docs/ROADMAP.md`, including the acceptance notes on what
is still unverified (desktop performance).

## Stack

- **C17**, no C++. Must build with GCC, Clang and MSVC (`/std:c17`).
- **SDL3** for the window, input, 2D renderer, timing, threads and atomics. Do not use C11
  `<threads.h>` or `<stdatomic.h>`, because MSVC support for them is unreliable. Use the SDL3
  equivalents instead.
- **CMake ≥ 3.24.** SDL3 is fetched with `FetchContent` at a pinned release tag and linked
  statically. Setting `-DPR_USE_SYSTEM_SDL=ON` uses an installed SDL3 instead.
- Targets: Linux, Windows, macOS (desktop only).
- Adding any other dependency requires an entry in `docs/DECISIONS.md` first.

## Commands

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
./build/release/pixel-roguelike
```

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DPR_SANITIZE=ON   # ASan + UBSan (GCC/Clang)
cmake --build build/debug -j
ctest --test-dir build/debug -j4 --output-on-failure   # in parallel (one CTest test per test)
./build/debug/pr-tests creature   # only tests whose name contains "creature" (prints timings)
ctest --test-dir build/tsan -j4 -R thread_count     # the multithreaded tests, under TSan
```

```bash
./build/release/pr-bench --threads 4 --width 4096 --height 2048 --fill 0.7   # headless benchmark
```

```bash
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPR_TSAN=ON   # ThreadSanitizer
```

```bash
# Headless visual check (e.g. under Xvfb): deterministic 1 tick/frame, save the last frame
./build/release/pixel-roguelike --demo --frames 300 --screenshot out.bmp
```

CMake options: `PR_SANITIZE` (ASan+UBSan), `PR_TSAN` (ThreadSanitizer), `PR_NATIVE`
(`-march=native`, off by default), `PR_USE_SYSTEM_SDL`, `PR_BUILD_TESTS` (on), `PR_BUILD_BENCH`
(on), `PR_WERROR` (CI).

On Linux, building SDL3 from source needs the X11/Wayland development packages listed in
`README.md`. The first configure downloads SDL (about 25 s to build on 4 cores).

## Doc map

| Doc | Read it when |
|---|---|
| `docs/VISION.md` | You need the game's intent, pillars, and open design questions |
| `docs/ROADMAP.md` | You pick up work. It lists milestones, scope and acceptance criteria |
| `docs/ARCHITECTURE.md` | You touch code structure, the main loop, data layout, threading or rendering |
| `docs/SIMULATION.md` | You touch cell rules: movement, density, heat, phase changes, gases |
| `docs/ELEMENTS.md` | You add or tune an element or its physical properties |
| `docs/PERFORMANCE.md` | You touch the hot path, or you need budgets, techniques, benchmarking, or the benchmark log |
| `docs/CODE_STYLE.md` | You write any C code |
| `docs/DECISIONS.md` | You make, or question, an architectural decision |

## Architecture in 30 seconds

- The **world** is a grid of cells, split into **64×64 chunks**. Each chunk keeps a *dirty rect*,
  and only dirty regions are simulated. Settled matter sleeps.
- The simulation runs at a **fixed 60 ticks/s**, decoupled from rendering. Each tick runs a
  movement pass (cellular automaton, bottom-up, with an alternating horizontal scan direction) and
  then a heat pass.
- **Parallelism** uses a 4-phase checkerboard of chunks. In each phase a worker owns one chunk and
  may write up to 32 px into its neighbours. That is why **a cell may never move more than
  `CHUNK_SIZE/2` = 32 px in one tick**.
- **Data layout** is a hybrid structure-of-arrays: a 4-byte `Cell` (element, flags, shade, aux) plus
  separate `float` temperature buffers, a per-cell gas mixture (`GasMix`, kg of each gas species)
  and a cached gas density. Element properties live in a
  lookup table indexed by element ID.
- **Rendering**: dirty chunks are converted to RGBA and uploaded to an SDL streaming texture, which
  is scaled to the window. The toolbar and HUD are a small hand-rolled immediate-mode UI.
- **Player edits** (brush strokes) are queued and applied on the main thread *between* ticks, never
  during one.

Details: `docs/ARCHITECTURE.md`.

## Source layout

```
src/core/     common.h (macros, PR_ASSERT, aligned alloc, timer), rng.h, log.h, jobs (thread
              pool). SDL threads/atomics allowed
src/sim/      elements (definition table + lookup tables), world (grid, chunks, dirty rects),
              sim (movement rules, gas mass and pressure, convection, tick), pressure (water
              bodies, transfers, jets), heat (conduction, phase changes), particle (free
              particles, bullet flight), creature (bodies, breathing, the player's body), weapon
              (the rifle), edit (brush strokes, edit queue, small clumps), scene (box, level,
              demo, bench).
              HEADLESS: no SDL video/render/events
src/render/   world -> texture (colour LUT; visible dirty chunks, converted in parallel)
src/ui/       immediate-mode widgets (button, panel, text)
src/game/     app: input, player control, toolbar, brush strokes, camera, HUD, fixed-timestep
              frame
src/main.c    argument parsing, SDL init, main loop
tests/        behaviour tests (CTest) on tiny worlds; test_util.c builds worlds from ASCII
bench/        pr-bench, headless benchmark
docs/         design and engineering docs
```

`src/sim` and `src/core` build as a static library (`pr_sim`) that the game, the tests and the
benchmark all link.

## Rules

### Hot path (anything that runs per cell per tick)

- No allocation, logging, SDL calls, or locks. No calls through function pointers per cell;
  dispatch with a `switch` on the element's phase/behaviour.
- Read element properties from the property tables (`g_elem_density[id]`, etc.), which fit in L1.
  Do not chase pointers.
- Movement rules use integer and comparison logic, with densities from the quantised table. Most
  floating point belongs in the heat pass. The exception is fluid buoyancy and convection, which
  read temperatures (with an integer fast path when temperatures are equal).
- Use `elem_temp(e, t)` for any physical use of a temperature (`temp` also holds latent heat).
- Never iterate the whole world when only dirty chunks or rects need work.
- Never let a cell move, or a rule read, more than 32 px from the cell being updated (the
  threading invariant above). Do multi-cell moves as one swap to the destination.
- Rules run in parallel. They may write only cells within reach, and wake only through the job's
  `TickCtx` (`ctx_wake`, `keep_alive`); never write chunk metadata or any shared state directly.
  Per-job state that is written often must live on the job's stack (false sharing).
- After any threading change, run the tests under ThreadSanitizer (`PR_TSAN`).
- Randomness comes only from the per-chunk RNG, seeded from `(world_seed, tick, chunk_x, chunk_y)`.
  Never use `rand()`, and never let worker threads touch a global RNG. Results must be identical
  for any thread count.
- Any change to data layout or the inner loop needs before/after numbers from `pr-bench` (Release
  build, same machine), recorded in `docs/PERFORMANCE.md` → Benchmark log.

### Code

- Follow `docs/CODE_STYLE.md`. The short version: C17, 4-space indent, `#pragma once`,
  `module_verb()` function names, `PascalCase` types, `UPPER_SNAKE` constants and enum values,
  fixed-width integer types, no VLAs, and `PR_ASSERT` for invariants.
- `src/sim/` must not include SDL video/render/event headers. It may use `src/core/`, which wraps
  SDL threads and atomics.
- Keep it simple. Add no abstraction layer until a second user of it exists.

### Docs are part of the change

- If you change behaviour, element values, architecture, controls or build commands, update the
  matching doc **in the same commit**.
- Record a new architectural decision, or a reversal of one, as an entry in `docs/DECISIONS.md`.
- Tick the roadmap checkboxes in `docs/ROADMAP.md` when items are done.

### Workflow

- Use Debug + `PR_SANITIZE` for correctness work. Use Release or RelWithDebInfo for performance
  work. **Never quote performance numbers from a Debug build.**
- Run `ctest -j4` (Release, then the ASan build) before committing. Run `pr-bench` before
  committing changes to `src/sim/`. While iterating, run only the tests you touch
  (`pr-tests <name>`); the full suites take about 12 s (Release) and 1 min (ASan) on 4 cores.
- After threading changes, run the `thread_count` tests under TSan (`-R thread_count`).
- Keep tests fast: the smallest world that shows the behaviour (sizes round up to 64) and only
  the ticks it needs. `pr-tests` prints each test's time; a test over a few seconds in Release
  needs a reason (CTest runs the slowest last otherwise; it is the critical path).
- Benchmark comparisons: build the old commit's `pr-bench` too and run old and new alternately
  (two runs each); `--ticks 200` is enough for the default scene.
- Mind the world size in tests: `world_create` rounds width and height up to multiples of 64
  (a "128 × 96" box is really 128 × 128).
- Write a behaviour test for every new rule: set up a tiny world, run N ticks, assert the
  outcome. Examples: water ends up below oxygen; the per-element cell count is conserved.
