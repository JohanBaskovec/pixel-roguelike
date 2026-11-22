# CLAUDE.md

Guidance for Claude Code (and humans) working in this repository. Read this file first, then the
doc that matches your task (see the doc map below).

## What this project is

A 2D, side-view game where **every pixel is simulated**. It draws on *Oxygen Not Included* (gases,
temperature, pressure, a colony underground) and *Noita* (a falling-sand world where everything is
destructible). The working title is the repo name, **pixel-roguelike**. Most of the game design is
still open (see `docs/VISION.md`). The **simulation engine** comes first.

### Non-negotiables

1. **Every pixel is a cell.** Each cell holds exactly one element (void, oxygen, CO2, hydrogen,
   water, dirt, rock, iron, ice, ...) and has its own temperature. Its physical properties
   (density, heat capacity, conductivity, phase) come from that element.
2. **No tiles.** All player interaction (painting, digging orders, building) goes through a
   pixel-precise brush that can be any size.
3. **Performance is a feature.** The world holds millions of cells. Any change to the simulation's
   hot path is judged by its benchmark result.
4. **Plausibly realistic.** Real-world physical values are the reference. Every deviation is
   deliberate, explicit (a named constant or scale factor), and documented in `docs/ELEMENTS.md` or
   `docs/SIMULATION.md`.
5. **Keyboard and mouse.**

## Current status

Documentation only; no code exists yet. The next milestone is **M1 – Sandbox box**: a box with dirt
walls, filled with void, plus toolbar buttons to paint gases, walls and water. See
`docs/ROADMAP.md`.

## Stack

- **C17**, no C++. Must build with GCC, Clang and MSVC (`/std:c17`).
- **SDL3** for the window, input, 2D renderer, timing, threads and atomics. Do not use C11
  `<threads.h>` or `<stdatomic.h>`, because MSVC support for them is unreliable. Use the SDL3
  equivalents instead.
- **CMake ≥ 3.24.** SDL3 is fetched with `FetchContent` at a pinned release tag and linked
  statically. Setting `-DPR_USE_SYSTEM_SDL=ON` uses an installed SDL3 instead.
- Targets: Linux, Windows, macOS (desktop only).
- Adding any other dependency requires an entry in `docs/DECISIONS.md` first.

## Commands (available once M1 lands)

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
./build/release/pixel-roguelike
```

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DPR_SANITIZE=ON   # ASan + UBSan (GCC/Clang)
cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure
```

```bash
./build/release/pr-bench            # headless simulation benchmark, prints ns/cell and ms/tick
```

Planned CMake options: `PR_SANITIZE` (ASan+UBSan), `PR_NATIVE` (`-march=native`, off by default),
`PR_USE_SYSTEM_SDL`, `PR_BUILD_TESTS` (on), `PR_BUILD_BENCH` (on).

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
  separate `float` temperature buffers. Element properties live in a lookup table indexed by
  element ID.
- **Rendering**: dirty chunks are converted to RGBA and uploaded to an SDL streaming texture, which
  is scaled to the window. The toolbar and HUD are a small hand-rolled immediate-mode UI.
- **Player edits** (brush strokes) are queued and applied on the main thread *between* ticks, never
  during one.

Details: `docs/ARCHITECTURE.md`.

## Source layout (planned)

```
src/core/     common types, asserts, RNG, thread pool, profiler (SDL threads/atomics allowed)
src/sim/      world, chunks, elements, movement rules, heat. HEADLESS: no SDL video/render/events
src/render/   world -> texture, overlays
src/ui/       immediate-mode widgets, toolbar, HUD
src/game/     input, tools (brush), edit queue, app state
src/main.c    SDL init, main loop
tests/        unit and behaviour tests (CTest), on tiny worlds
bench/        headless benchmark executable
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
  floating point belongs in the heat pass. The one planned exception is fluid buoyancy reading
  temperature (convection, M3).
- Never iterate the whole world when only dirty chunks or rects need work.
- Never let a cell move more than 32 px per tick (the threading invariant above).
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
- Run `ctest` before committing. Run `pr-bench` before committing changes to `src/sim/`.
- Write a behaviour test for every new rule: set up a tiny world, run N ticks, assert the
  outcome. Examples: water ends up below oxygen; the per-element cell count is conserved.
