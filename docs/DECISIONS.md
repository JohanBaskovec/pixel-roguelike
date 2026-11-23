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

- **Status:** Accepted for M1–M3; revisited at M4: one element per cell kept, gases got mass
  (D-024)
- **Context:** Noita uses one particle per pixel. ONI uses a mass per cell plus pressure. Particles
  are simpler and fast. Mass and pressure are more realistic: gas fills a vacuum and thins out, and
  pressure exists.
- **Decision:** Each cell holds exactly one element. Gases start as particles (M1). The data layout
  reserves a per-cell `mass` array, so M4 can add mass and pressure without a re-layout.
- **Consequences:** Until M4, a little gas painted into a large vacuum spreads as sparse dots
  instead of a thin uniform gas. This is accepted for now.
- **M4 outcome:** Kept one element per cell, and added the `mass` array as planned. Mixture cells
  (several gases per cell) would make partial pressures, breathing and total-pressure balance
  exact, but cost memory per cell and complicate every rule and the renderer. The price of
  keeping it: total pressure only evens out across two gases when one pushes the other, mixing is
  a thin interface layer, and O2/N2 must share a buoyancy density so painted air stays mixed
  (see `SIMULATION.md` → Gases, limitations). Revisit if breathing (M4c) needs exact partial
  pressures everywhere.

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
- **Amendment 2:** A falling cell blocked by its own falling liquid waits in mid-air instead of
  landing, because poured streams bulged below every chunk border. Test:
  `test_falling_stream_has_no_chunk_bulges`.
- **Amendment 3:** That wait applies only if the falling column below ends in open space; in a
  real jam the cell lands. Pressured cells now look through any of their own liquid (not just
  falling cells) up to 32 cells for open space. Together these stop a stream poured beside a wall
  from stacking into a standing tower. Test: `test_pour_against_wall_makes_no_tower`.
- **Superseded in part by D-018 (M2b):** pressured cells no longer flow on their own (water
  pressure moves water to openings), and the surface wander is removed. The drop rule, looking
  through its own liquid, and waiting behind falling streams remain.

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
- **Amendment (implementation, M2b):** The head field was built and abandoned: values left over
  from a dropped surface bounced between chunks and decayed slowly (a small propagation cost made
  them linger for seconds, a large one made big pools imprecise). Implemented instead: still
  liquid is labelled into connected bodies (flood fill per changed chunk, union-find across chunk
  borders every tick); each body knows its top row; a still cell next to an opening at least one
  row below its body's top requests a transfer, carried out serially after the phase by moving a
  free-surface cell from the top to the opening. Exact, no stale values, any distance, cheaper.
  One byte per cell (piece label) instead of a float. Details in `SIMULATION.md` → Liquid
  pressure.

## D-019 — Parallel phases with per-job wake buffers

- **Status:** Accepted (M2); resolves the open choice in D-006
- **Context:** In a parallel phase, a job's moves near its chunk border wake cells in neighbouring
  chunks. Up to four jobs can touch the same neighbour's dirty rect. The options were atomic
  min/max on the shared rects, or buffering.
- **Decision:** Each job has a wake buffer for the 3×3 chunks around its own. The main thread
  merges the buffers after the phase. The thread pool is a simple fork-join pool on SDL threads
  (atomic job counter; the caller works too). Jobs work on a stack copy of their context.
- **Consequences:** There are no atomics or locks in the hot path, and the merge is cheap (at most
  9 rects per job). The result is independent of thread count (tested). Lesson recorded in
  `ARCHITECTURE.md`: per-job state that sits side by side in memory and is written constantly must
  be thread-local, or false sharing eats the speed-up.

## D-020 — Rendering: world-sized texture, visible dirty chunks, row-span uploads

- **Status:** Accepted (M2)
- **Decision:** Keep one world-sized streaming texture. Each frame, convert only render-dirty chunks
  inside the view (in parallel), and upload one rect per chunk row spanning its dirty chunks.
  Sampling is nearest-neighbour when zoomed in, linear when zoomed out.
- **Consequences:** Off-screen activity costs no rendering. The texture needs 32 MB of GPU memory
  at 4096 × 2048, and 4096 px textures are supported by all current GPUs. Worlds beyond the GPU's
  maximum texture size (often 16384) will need tiling (M6, world streaming).

## D-021 — Latent heat via stored (enthalpy) temperature

- **Status:** Accepted (M3)
- **Context:** Phase changes need latent heat: water at 100 °C absorbs 2257 kJ/kg before boiling.
  Options were a separate per-cell "phase progress" buffer, or folding it into the temperature.
- **Decision:** The stored temperature doubles as heat content. Past a transition point the excess
  is latent heat; `elem_temp()` gives the physical temperature, clamped at the transition point,
  for conduction, buoyancy and display. A cell changes phase once `c·|T − T_thr| ≥ L` and keeps the
  remainder.
- **Consequences:** No extra memory, energy is conserved, and mixtures hold at 0 °C / 100 °C while
  changing phase, as in reality. Stored temperatures can look odd (boiling water stored at
  ~900 K, condensing steam stored below 0 K), so code must use `elem_temp()` for physics and
  display.

## D-022 — Smaller heat speed-up for gases

- **Status:** Accepted (M3); refines D-008
- **Context:** With conduction sped up 1000× everywhere, hot air diffused away before buoyancy
  (which runs in real time) could lift it.
- **Decision:** `HEAT_TIME_SCALE` = 1000 for solids and liquids, `HEAT_TIME_SCALE_GAS` = 10 for
  any contact involving a gas.
- **Consequences:** Hot gas rises; solids heat at a playable pace. Air heats more slowly from hot
  surfaces, which convection and moving air make up for. Both are tunable constants, listed as
  deviations in `ELEMENTS.md`.

## D-023 — Heat runs only where temperatures differ

- **Status:** Accepted (M3)
- **Context:** Waking the heat pass wherever cells moved cost +80% on the movement benchmark, even
  with the whole world at one temperature.
- **Decision:** Chunks keep a temperature range, widened by arriving cells (in the movement pass
  and transfers) and recomputed by the heat pass. Arrivals reactivate heat only if the range of
  the chunk and its neighbours is wider than `HEAT_UNIFORM_K`. Edits and phase changes activate
  heat directly.
- **Consequences:** A uniform-temperature world costs almost nothing (+8% on the benchmarks, from
  convection and buoyancy checks). The range is conservative: it only shrinks when the heat pass
  recomputes the chunk.

## D-024 — Gas mass: split, push, equalise, mix by fewer contacts

- **Status:** Accepted (M4); outcome of D-005
- **Context:** Gases need mass and pressure (fill a vacuum and thin out, P ∝ T in a sealed room,
  boiling keeps its mass) on a grid where each cell holds one gas, within the 32 px reach, in
  parallel, and able to sleep.
- **Decision:** Each gas cell holds a mass; pressure from the ideal gas law. Per tick, one random
  direction: dense gas splits half its mass into void (thin gas, under 2% of 1 atm, moves as a
  particle and merges with thin gas of its kind); a gas at more than 2.1× a neighbouring gas
  cell's pressure pushes it into the next cell and expands into its place; cells of the same gas
  trade mass to equal pressure, with a partner up to 16 cells away through any gas when their
  temperatures are within 2 K (else the adjacent cell); different gases trade places only when
  that lowers their number of same-gas contacts and does not lift the denser one. Buoyancy uses
  densities at 1 atm. O2 and N2 share the density of air for buoyancy.
- **Consequences:** Mass is conserved exactly; P ∝ T holds; vacuum fills in seconds; a room of
  gas at one pressure, mixed air included, sleeps. Pressure spreads by diffusion (seconds per
  room, not the speed of sound). Moving gas between cells at different temperatures mixes their
  heat, so suddenly heated gas rises less sharply. The benchmark's random gas soup got ~2.6×
  slower (more gas cells after filling the vacuum, and heavier rules; `PERFORMANCE.md`).

## D-025 — Gas pressure on liquids as height; compression spread over regions

- **Status:** Accepted (M4)
- **Context:** Water pressure (D-018) compares heights. Gas pressure must push on liquids
  (pressurised tanks, barometers, trapped air), and liquid entering gas must compress it, without
  a new solver.
- **Decision:** Express gas pressure as a height of the liquid (1 atm = 207 cells of water). A
  surface cell's head is its row minus the gas above it; an opening's head is its row minus the
  gas in it; the body's top is the smallest surface head, and the M2b rule is unchanged. Liquid
  entering gas trades places with it when both gases are at about one pressure (one region);
  otherwise it compresses the gas, and the gas above the source expands, each spread over up to
  256 cells of that gas (breadth-first through gas). Such transfers are limited to one per body
  every 2 ticks. Different gases balance total pressure the same way: a cell changes from the
  lower-pressure gas to the higher, both changes spread over the gases' regions.
- **Consequences:** Pressurised tanks, barometers and compressed trapped air work (tested), and a
  world at one pressure behaves exactly as before. Single-cell compression was tried first: one
  cell's volume at one spot is tens of percent of its pressure (×207 cells of head), and the
  water churned without end. Spreading needs a breadth-first walk per compressing transfer and
  per balance (cost: +12% on the gas-soup benchmark, almost all from balancing). Gas-driven flow
  is slower than real (rate limit), and pressures are resolved to one cell of liquid.

## D-026 — Free particles for jets and splashes; falling streams stay on the grid

- **Status:** Accepted (M4b)
- **Context:** Water leaving a hole should jet out at √(2·g·depth) and arc; falling water that
  hits something should splash. Grid cells move at most 32 cells per tick and only along the
  automaton's rules.
- **Decision:** A bounded list of free particles (position and velocity in floating point),
  updated serially in list order at the end of each tick, landing back into the grid when they
  hit something. Water pressure launches jets from deep, covered side openings (about `speed`
  cells per tick); fast landings splash with probability 1/4. Falling streams keep the grid rules
  (D-014); jobs in the parallel pass only queue splashes, added in job order after each phase.
- **Consequences:** Torricelli outflow (measured within 5%) and ballistic arcs; results identical
  for any thread count. The serial particle update is cheap at the counts seen (hundreds). The
  gas-soup benchmark got 13% slower, mostly from splashes leaving holes that wake neighbours.
  Flow through a submerged opening is still not limited by its size (the body is joined).

## D-027 — Creatures are entities stamped into the grid as solid body cells

- **Status:** Accepted (M4c)
- **Context:** Characters need their own position, shape and physics, but the grid's matter must
  feel them: gas and water pushed aside, heat exchanged, air breathed.
- **Decision:** A creature is an entity (position, velocity, state) whose body is stamped into the
  grid as `ELEM_BODY` cells (static to every grid rule). It moves one cell at a time, swapping the
  matter its leading edge enters with the body cells it leaves, so matter is conserved exactly.
  Creatures update serially at the start of each tick. Breathing takes and adds exact masses near
  the mouth and stirs the air around it (swapping gas cells near and far), because gases otherwise
  mix only at interfaces (D-005).
- **Consequences:** No grid rule needed changes: walls, water pressure, gas and particles handle
  bodies already. Determinism holds for any thread count (tested). Matter displaced by a step
  teleports across the body (9 or 35 cells), which reads as flowing around it. Stirring is an
  explicit modelling choice for air currents (documented as a deviation in `ELEMENTS.md`).

## D-028 — Liquids push each other through the load above them

- **Status:** Accepted (M5); extends D-018 and D-025
- **Context:** With several liquids (oil, lava, molten iron), water pressure must compare
  pressures, not levels: a column of oil weighs on the water under it.
- **Decision:** The "head" of a surface cell or an opening subtracts the load above it as a height
  of the liquid: every liquid cell stacked above, by its density over the liquid's, plus the gas
  pressure over that column (bubbles of up to 2 empty cells do not end the column). Liquid
  entering a lighter liquid lifts that liquid's column by one cell instead of swapping it to the
  source.
- **Consequences:** Manometers work (tested within one cell). Reading the column costs a scan
  upward: unlimited in labelling and transfers (nothing else writes then), limited to the 32-cell
  reach in the parallel request check, which the transfer then confirms. A first version that
  stopped at the first non-lighter cell, and swapped lighter liquid to the source, let oil leak
  into the far arm of a U-tube.

## D-029 — Gas currents: a direction and speed per gas cell, in its aux byte

- **Status:** Accepted (M5)
- **Context:** Play-testing showed the air never moving: bodies and falling matter swapped only
  the cells they touched, and mixed air slept. Real air is dragged along (wakes, entrainment).
- **Decision:** Gas cells keep a current (3-bit direction, 5-bit speed) in `aux`, which gases did
  not use. A current moves its cell along, hands part of itself to the cell it pushes and to a
  neighbour, turns at walls and fades in about 2 s. Creatures' steps and breaths, and falling
  liquid and powder, create currents. No new memory; no solved flow field.
- **Consequences:** Walking and falling matter visibly stir and mix gases, with mass conserved and
  results identical for any thread count. The gas-soup benchmark did not get slower (14–15 ms;
  its mixtures settle sooner). Currents do not arise from pressure differences or heat; buoyancy
  and pressure equalisation still do that work.

