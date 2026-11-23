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
- ~~Atomics vs per-worker deferred lists for cross-chunk dirty marking~~: per-job buffers (D-019).
- SIMD for the heat pass.
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
- **Races:** build with `-DPR_TSAN=ON` (ThreadSanitizer) and run `pr-tests` and a few
  `pr-bench --threads 4` ticks after any threading change.
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
