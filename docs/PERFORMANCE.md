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
- **Races:** build with `-DPR_TSAN=ON` (ThreadSanitizer) and run the multithreaded tests
  (`ctest -R thread_count`) and a few `pr-bench --threads 4` ticks after any threading change.
- **Tests' speed:** each test is its own CTest test (`ctest -j4` runs them in parallel), the
  sanitizer build compiles at `-O1`, and tests use the smallest world and fewest ticks that show
  their behaviour. Release: 75 s → 12 s; ASan + UBSan: about 14 min → 1 min (4 cores, M6a).
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
