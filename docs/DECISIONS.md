# Decision log

Architecture decisions, newest at the bottom. Never delete an entry. To reverse a decision, add a
new entry that supersedes it, and mark the old one `Superseded by D-xxx`.

Format: **Context** (why the decision came up), **Decision**, **Consequences** (what it costs, what
to watch).

---

## D-001 — Language: C17

- **Status:** Accepted (project owner's choice)
- **Context:** The simulation must handle millions of cells, and we want full control of memory
  layout and threading.
- **Decision:** Plain C17, no C++. It must compile on GCC, Clang and MSVC.
- **Consequences:** Memory is managed by hand and there are no generics. Keep the code simple and
  table-driven. Use ASan/UBSan in Debug from day one. Avoid C11 threads and atomics (MSVC support
  is unreliable) and use SDL3 equivalents instead.

## D-002 — Platform layer: SDL3

- **Status:** Accepted (project owner's choice)
- **Decision:** SDL3 provides the window, input, timer, threads, atomics and 2D renderer. M1
  renders with `SDL_Renderer` and a streaming texture.
- **Consequences:** SDL3 is portable and well maintained. When shaders become necessary (glow,
  lighting, smooth gas), move rendering to the SDL3 GPU API. That needs a new decision entry.

## D-003 — Build: CMake + FetchContent (pinned SDL3)

- **Status:** Accepted
- **Decision:** CMake ≥ 3.24. SDL3 comes from `FetchContent` at a pinned `release-3.x.y` tag and
  is linked statically. `PR_USE_SYSTEM_SDL=ON` uses an installed SDL3 instead.
- **Consequences:** The first configure downloads and builds SDL, which takes a minute or two.
  Builds are reproducible. Bumping the SDL tag is a deliberate commit. Choose the latest stable
  SDL3 release when the build is first written.

## D-004 — Simulation on the CPU, not the GPU

- **Status:** Accepted
- **Context:** GPU compute could push more raw cells, but falling-sand rules are branchy,
  order-dependent and full of interactions (reactions, phase changes, future gameplay hooks). They
  are hard to write, debug and keep deterministic on the GPU.
- **Decision:** A multithreaded CPU simulation over chunks, as Noita does. The GPU is used only
  for presentation.
- **Consequences:** Throughput depends on sleeping chunks and core count, so the performance work
  in `PERFORMANCE.md` is mandatory. A GPU path could later handle embarrassingly parallel passes
  (for example heat) if profiling demands it.

## D-005 — One element per cell; particle gases first, mass and pressure later

- **Status:** Accepted for M1–M3; revisit at M4
- **Context:** Noita uses one particle per pixel. ONI uses a mass per cell plus pressure. Particles
  are simpler and fast. Mass and pressure are more realistic: gas fills a vacuum and thins out, and
  pressure exists.
- **Decision:** Each cell holds exactly one element. Gases start as particles (M1). The data layout
  reserves a per-cell `mass` array, so M4 can add mass and pressure without a re-layout.
- **Consequences:** Until M4, a little gas painted into a large vacuum spreads as sparse dots
  instead of a thin uniform gas. This is accepted for now.

## D-006 — Chunks of 64×64, dirty rects, 4-phase checkerboard

- **Status:** Accepted (threading lands in M2)
- **Decision:** The world is divided into 64×64 chunks, each with a current and a next dirty
  rect. Parallel updates run in 4 checkerboard phases. **A cell may move at most 32 px per tick.**
- **Consequences:** Every movement rule must respect the 32 px limit. World dimensions are
  multiples of 64.

## D-007 — Fixed timestep, 60 ticks/s

- **Status:** Accepted
- **Decision:** The simulation runs at a fixed 60 Hz, decoupled from rendering, with at most 4
  ticks per frame. If the simulation cannot keep up, it slows down rather than spiralling.
- **Consequences:** The simulation is deterministic and independent of frame rate. Interpolating
  visuals between ticks is unnecessary for pixel art.

## D-008 — SI units, Kelvin, real reference values

- **Status:** Accepted
- **Decision:** Internal units are SI, and temperature is in Kelvin. Element properties start
  from real-world values. Deviations are explicit global factors or per-element overrides, listed
  in `ELEMENTS.md`. The working cell size is 5 cm.
- **Consequences:** Behaviour is believable and makes sense to anyone familiar with physics. Heat
  needs `HEAT_TIME_SCALE` to be playable.

## D-009 — Hand-rolled immediate-mode UI for now

- **Status:** Accepted; revisit when the UI grows
- **Decision:** Toolbar, buttons and HUD are drawn with SDL rects and `SDL_RenderDebugText`. No UI
  library.
- **Consequences:** There are no dependencies and the UI is enough for M1–M3. When menus,
  scrolling lists or proper fonts are needed, evaluate Nuklear or microui (single-file C) and
  SDL3_ttf.

## D-010 — Determinism independent of thread count

- **Status:** Accepted
- **Decision:** Each chunk job gets its own RNG, seeded from `(seed, tick, cx, cy)`. Scan order
  within a chunk is fixed, and the heat pass is double-buffered (Jacobi).
- **Consequences:** Bugs can be reproduced, and replays are possible from seed + edit stream
  (useful for tests, and maybe for saves or multiplayer later). This rules out work-stealing inside
  a chunk and any shared mutable RNG.

## D-011 — Hybrid SoA data layout

- **Status:** Accepted; revisit with benchmarks in M2
- **Decision:** A 4-byte `Cell {element, flags, shade, aux}` in a row-major array. Temperature is
  in separate double-buffered `float` arrays, and `mass` gets its own array in M4.
- **Consequences:** The movement pass is cache-friendly, and the heat pass streams cleanly. Moving
  a cell means swapping 2–3 arrays. Chunk-major layout and full SoA are open experiments.

## D-012 — Headless simulation library

- **Status:** Accepted
- **Decision:** `src/sim` + `src/core` build as the static library `pr_sim`, with no SDL
  video/render/event usage. The game, tests and benchmark link against it.
- **Consequences:** Tests and benchmarks run in CI without a display. This keeps simulation logic
  out of rendering and UI code.

## D-013 — 8-bit tick stamp instead of a 1-bit clock

- **Status:** Accepted (M1)
- **Context:** A 1-bit clock, flipped every tick, marks cells already updated in this tick. A cell
  that slept for an odd number of ticks wakes with the "already updated" bit set half the time.
  It then skips a tick, which tears falling columns into stripes.
- **Decision:** `Cell.stamp` holds the low 8 bits of the tick in which the cell was last updated
  or moved. Painted cells get `tick - 1`.
- **Consequences:** A woken cell is wrongly skipped only if it last moved a multiple of 256 ticks
  ago (1 in 256), and then only for one tick. This costs no memory, since the byte was reserved as
  flags.

## D-014 — Liquid rules shaped by sleeping and chunk scanning

- **Status:** Accepted (M1)
- **Context:** Classic falling-sand liquid rules either jitter forever (pools never sleep) or pile
  up like sand. Scanning the world chunk by chunk also made a falling stream just across a chunk
  border act as a wall, so spreading water froze at chunk borders.
- **Decision:** Pressured cells (liquid above them) flow into any open space beside them. Surface
  cells flow only toward a drop within 32 cells, otherwise wander occasionally. Pressured cells may
  pass through falling cells of their own liquid. Details in `SIMULATION.md` → Liquids.
- **Consequences:** Pools level to within one cell and their interiors sleep. The surface row
  shimmers and stays awake. Behaviour no longer depends on where chunk borders fall (covered by
  manual checks; a dedicated test is a good M2 addition).
- **Amendment:** Surface cells look through their own liquid when searching for a drop, so the
  whole top row of a step flows down, not just its edge cell. Poured water had been piling into a
  stepped mound. Test: `test_poured_water_spreads`.

## D-015 — Rotate the checkerboard phase order every tick

- **Status:** Accepted (M1); amends D-006
- **Decision:** The first of the 4 phases is `tick & 3`.
- **Consequences:** No chunk group always gets first pick of the free space at its borders. The
  result is still deterministic for any thread count.

## D-016 — SDL3 3.4.18, with unused subsystems disabled

- **Status:** Accepted (M1); refines D-003
- **Decision:** Pin `release-3.4.18`. Build SDL without the audio, camera, joystick, haptic,
  HIDAPI, sensor and power subsystems.
- **Consequences:** The build is faster and smaller. Re-enable a subsystem when a feature needs it
  (audio, for example). On Linux, building SDL needs the X11/Wayland development packages listed
  in `README.md`.

## D-017 — No heat radiation: vacuum is a perfect insulator

- **Status:** Accepted (project owner's choice)
- **Context:** Real objects lose heat by radiating infrared light, even through vacuum. This
  matters most at high temperatures. Modelling it costs from cheap (losing heat to the
  surroundings) to very expensive (surfaces exchanging heat with every surface they can see).
- **Decision:** No radiation. Heat moves only by conduction between touching cells and by moving
  matter (convection). Void holds no heat and passes none.
- **Consequences:** A layer of vacuum insulates perfectly (as in ONI, where this is a known player
  strategy). A hot object surrounded by vacuum never cools. Revisit only if gameplay needs hot
  things to cool in the open.

## D-018 — Water pressure as a pressure field over single-element pixels (Option A)

- **Status:** Accepted (project owner's choice); scheduled as milestone M2b
- **Context:** Liquid rules only look within 32 cells (threading invariant). So there is no
  hydrostatic pressure: U-tubes do not balance, dams push no harder than a puddle, and a poured
  pool keeps a gentle slope of about one cell per 32 cells. Surfaces also need a random "wander"
  that makes them shimmer. The alternative considered was ONI-style liquid *amounts* per cell
  (Option B). It was rejected because it breaks "every pixel is one element" and needs
  partial-cell rendering.
- **Decision:** Keep one element per pixel. Add a per-cell pressure (hydraulic head) field for
  liquids, updated by a local relaxation pass that only reads within 32 cells. Liquid moves
  toward lower head, including upward when the head is high enough. Design notes are in
  `SIMULATION.md` → Liquid pressure.
- **Consequences:** One extra float per cell (to be shared with the M4 gas mass) and one extra
  pass over active liquid. Pressure travels a bounded distance per tick, so very large bodies
  balance over seconds rather than instantly. The surface wander, and with it the shimmer, goes
  away.
