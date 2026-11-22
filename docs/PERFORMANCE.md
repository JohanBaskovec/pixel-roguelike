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
| M2 | 4096 × 2048 (8.4 M cells), multithreaded | 60 ticks/s with ≈ 2 M active cells |

## Techniques we rely on

Ordered by impact.

1. **Sleeping chunks and dirty rects.** Do no work where nothing changes. This is the biggest win
   by far. Every new rule must leave settled cells in a state where they stop marking themselves
   dirty. For example, a gas random walk into the same gas must not mark the chunk dirty.
2. **Multithreading.** A 4-phase checkerboard over chunks (`ARCHITECTURE.md`). Use one worker per
   core minus one. Jobs are whole chunks, so per-job overhead stays small.
3. **Compact cells.** A 4-byte `Cell` gives 16 cells per cache line. Temperature lives in a
   separate array so the movement pass does not drag it through the cache except when swapping.
4. **Table-driven properties.** Element properties are flat arrays indexed by element ID, small
   enough to stay in L1. Do not chase pointers per cell.
5. **Branch-light inner loops.** Skip `STATIC` and `NONE` with one table lookup. Dispatch the
   remaining behaviours through a `switch`, never through function pointers.
6. **Integer movement logic.** Quantised densities and an integer RNG. No floating point in the
   movement pass, apart from M3 fluid buoyancy (convection).
7. **Upload only dirty chunks** to the texture.
8. **No allocation after startup.** All buffers are allocated once and aligned to 64 bytes.
   Per-chunk metadata is padded to cache lines to avoid false sharing between workers.
9. **Heat at reduced frequency where possible** (every N ticks with a scaled dt), and only on
   heat-active chunks.

Planned experiments, decided by data only:
- A permanent 1-cell wall ring at the world edge, so neighbour reads need no bounds check.
- Chunk-major memory layout instead of row-major.
- Full SoA for `Cell`.
- Atomics vs per-worker deferred lists for cross-chunk dirty marking.
- SIMD for the heat pass.
- `SDL_LockTexture` vs `SDL_UpdateTexture` for uploads.

## Measuring

- **`pr-bench`** (headless) builds a deterministic scenario: a world filled with a churning mix of
  water and three gases over a dirt-and-rock floor. It runs a warm-up, then N ticks, and prints:
  - ms/tick at p50, p99 and max
  - updated cells per tick
  - **ns per updated cell** (the main number to watch)
  - thread count, world size, build type, compiler
- **In-game HUD timers** (M2) are scoped timers built on `SDL_GetPerformanceCounter`, aggregated
  per frame: sim, heat, edits, upload, render.
- **External profilers:** `perf` / Hotspot (Linux), Instruments (macOS), and Superluminal or VS
  Profiler (Windows). Use a `RelWithDebInfo` build. Tracy may be added later; it needs a
  `DECISIONS.md` entry because it is a dependency.

Rules
- Quote only Release or RelWithDebInfo numbers, never Debug.
- Compare before and after on the same machine, with the same scenario and seed, in back-to-back
  runs.
- Every change to `src/sim/` hot code adds a line to the benchmark log below.

## Benchmark log

| Date | Commit | Machine (CPU, cores) | Compiler | World | Threads | ns/updated cell | ms/tick p50 / p99 | Change |
|---|---|---|---|---|---|---|---|---|
| 2026-11-22 | M1 | Cloud VM, Xeon @ 2.1 GHz (4 vCPU) | gcc 13.3 -O3 | 1024×768 bench scene | 1 | 62.2 | 13.6 / 19.3 | First M1 version |
| 2026-11-22 | M1 | same | gcc 13.3 -O3 | 1024×768 bench scene | 1 | 57.8 | 12.5 / 18.7 | Multi-cell moves as one swap; paired dirty-rect wake |
| 2026-11-22 | M1 | same | gcc 13.3 -O3 | 1024×768 bench scene | 1 | 58.3 | 12.7 / 19.1 | Final M1 liquid rules (pressure, lookahead, wander, streams): ~231k updated cells/tick |
| 2026-11-22 | M1 | same | clang 18 -O3 | 1024×768 bench scene | 1 | 58.1 | 14.0 / 17.3 | Same code, Clang |
| 2026-11-22 | M1+ | same | gcc 13.3 -O3 | 1024×768 bench scene | 1 | 59.8 | 13.1 / 19.2 | Surface liquid looks through its own liquid (no measurable change) |

Notes on the M1 baseline:
- The bench scene is a worst case: 65% of the box is a random mix of water, three gases and
  vacuum, so almost every cell moves every tick.
- This VM is slow: a trivial dependent-ALU loop runs about 2–3× slower than on a current desktop
  CPU. Expect roughly 20–30 ns/cell on a desktop.
- Callgrind attributes most of the time to swaps and dirty-rect bookkeeping. What remains is
  dominated by unpredictable branches (random walk directions), which is inherent to the
  automaton. The big lever is multithreading (M2).
- In the demo scene, the in-game HUD shows 2–5 ms/tick for 30–50k updated cells.
