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
  (D-024). **Superseded for gases by D-035 (M6a):** a gas cell holds a mixture.
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
  in `ELEMENTS.md`. The working cell size is 5 cm (confirmed in D-037).
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

## D-030 — Gas mixtures: bulk buoyancy instead of per-cell buoyancy between gases

- **Status:** Superseded by D-035 (M6a)
- **Context:** With one gas per cell, buoyancy between different gases acted cell by cell: every
  CO2 cell sank through every air cell. Mixtures unmixed into pure layers within seconds, and
  breathed-out CO2 rained onto the floor. A user asked why CO2 and oxygen never mix. In reality
  a mixed gas stays mixed (separation needs kilometres of height), and only a mixture that is
  denser in bulk (a layer, a blob, a plume) sinks.
- **Decision:** A gas cell among cells of other gases stands for gas mixed at the molecular scale.
  Between different gases, buoyancy compares the mean density of windows of cells (above vs
  below, and vs the lighter side; 4, 8 and 16 rows tall). They must differ by 2 standard errors,
  taken as if both held the same mixture, so a mixture's own noise never moves it, whatever its
  gases. A sinking parcel moves as a whole: the light cell is lifted to the parcel's top, in one
  rotation of up to 31 cells within reach. A heavy gas resting on something spreads sideways (a
  gravity current). Warmer cells still rise one by one (thermals). Diffusion is slow in still gas
  and fast where the gas has a current (turbulence).
- **Alternatives:** a composition per cell (several gases sharing a cell): far more memory and work
  per cell, against "one element per cell" (D-005). Per-cell sinking scaled by local purity:
  froze into checkerboards at interfaces, and blobs crumbled. Fixed contrast thresholds instead of
  the significance test: a threshold low enough for CO2 in air let cell-level mixtures of H2 and
  CO2 unmix. A coarse density field per chunk: blocky, and it needs a separate pass.
- **Consequences:** Mixtures stay mixed and sleep. Blobs sink and pool, and layers turn over.
  Breathed-out CO2 stays in the air, so CO2, not oxygen, is the first danger in a sealed room (as
  in reality). Air (O2/N2, one density) pays nothing. The gas-soup benchmark (a cell-level random
  mix of three gases, all awake) is 41–60% slower: the window sums (tried one tick in 2, all
  heights one tick in 16), and the soup no longer unmixes into calm layers, so more cells keep
  balancing pressure across gases. A thin CO2 haze over clean air does not sink.

## D-031 — Action roguelike: the player is a creature, bullets are free particles

- **Status:** Accepted (M6a)
- **Context:** The design question in VISION.md (colony sim or roguelike) was decided: an action
  roguelike with a player character and weapons that act on the simulation.
- **Decision:** The player is a creature (D-027) with its own body size (1.85 m, 37 × 9 cells),
  driven by the keyboard instead of wandering; it breathes and warms the air like the others. A
  bullet is a free particle (D-026) carrying a cell of lead. Fast, it flies its whole path in one
  tick (900 m/s: 300 cells, in half-cell steps), serially at the end of the tick like other
  particles, so it is not bound by the 32-cell reach of the parallel rules and stays deterministic.
  It digs by knocking cells out as debris particles (loosened: soil, rubble), so no pixel is lost.
  Slow, it is an ordinary particle that lands as a powder pixel. The rifle runs between ticks on
  the main thread. Clumps of at most 20 solid cells held by nothing break loose after digging.
- **Alternatives:** bullets as grid cells moving 300 cells per tick: impossible under the reach
  invariant. Hit-scan (no bullet): against the request that a real bullet flies and lands.
  Destroying dug cells: against the rule that no pixel is destroyed.
- **Consequences:** Shooting costs little (a few hundred cell checks per bullet, serial). Spent
  bullets and casings pile up as real matter. The camera follows the player at zoom 1. Next:
  simulate only near the player, HP and damage (ROADMAP M6b).

## D-032 — Gas diffusion follows concentration gradients at several scales

- **Status:** Superseded by D-035 (M6a); its diffusivity (2.5e-3 m²/s) is kept
- **Context:** Gases only mixed where a swap lowered the same-gas contacts next to the two cells.
  That evened out an interface a few cells deep and then stopped, so the inside of a clump
  never mixed and large pure clumps (a CO2 pool, a dilute wake) stayed forever. The user asked
  that no large clump stay unmixed.
- **Decision:** A gas cell trades places with a different neighbour when the fraction of its gas
  is clearly higher on its side than on the neighbour's, compared in windows 4 to 32 cells long
  (a pooled binomial test at 3 standard errors), with a probability proportional to the gradient
  divided by the share of pairs that can swap (Fick's law: the flow does not depend on the
  concentration). The rate stands for gentle indoor turbulence (about 2.5e-3 m²/s). The old
  contact rule stays, slower, for clusters too small to be a gradient. Stirred gas swaps at
  random in proportion to the current's speed. 8 × 8 gas block counts, recounted each tick for
  awake chunks, gate the precise test, so still air costs almost nothing.
- **Alternatives:** unbiased random swaps everywhere (true random-walk diffusion): mixtures would
  never sleep, and all awake air would churn. A composition per cell: against D-005. Currents
  around sinking gas (turbulent entrainment): tried, and it shredded blobs into standing columns.
- **Consequences:** Interfaces widen as √t, a CO2 ball lands, pools and keeps thinning, mixtures
  and still air sleep. The gas-soup benchmark is unchanged; the idle level costs a few ms more
  where the player's breath keeps gradients alive. A gradient shallower than about 1% per cell
  is left alone.

## D-033 — Creatures breathe the air in and out through lungs

- **Status:** Accepted (M6a)
- **Context:** A breath took only oxygen out of the oxygen cells near the mouth and put CO2 back:
  nitrogen, and any other gas, never went in or out. The user asked that every creature breathe
  in and out realistically, a mix of gases.
- **Decision:** Each creature has lungs (a mass per gas). Breathing in takes a tidal volume of the
  air as it is near the mouth (the same fraction of each of the nearest gas cells, whatever their
  gas). The body takes its oxygen from the lungs (at most half of what they hold) and adds CO2.
  Breathing out, 1.7 s later, returns every gas to the nearest cells of its kind at 34 °C, or as
  new cells. With the mouth in liquid it holds its breath and lives on the lungs' oxygen.
- **Alternatives:** keep the oxygen-only exchange (no nitrogen or other gases move, no warm
  breath); a composition per cell (against D-005).
- **Consequences:** Gas mass is conserved counting the lungs (tested). Breathing hydrogen or CO2
  returns it. The breath out warms the air in front of the face. The lungs hold only the tidal
  volume, so a held breath runs out after a few breaths.

## D-034 — Gas currents mix heat (turbulence)

- **Status:** Accepted (M6a)
- **Context:** The muzzle gas (1500 K) glowed for more than 2 s: gas cells only exchange heat by
  conduction (10× sped up), and the currents that mix gas only swap cells, which moves heat
  along without sharing it. Real turbulent air mixes heat in milliseconds over centimetres.
- **Decision:** In the heat pass, between two gas cells, the coefficient is multiplied by
  `1 + TURB_PER_WIND · speed` of the stronger current (an eddy diffusivity of one cell times the
  current's speed), still bounded by the stability limit. Only gas–gas contacts: a surface keeps
  its boundary layer.
- **Alternatives:** emit the muzzle gas already mixed and cooler (no flash, and every other
  current would still carry heat without sharing it); a one-off cooling rule for muzzle gas.
- **Consequences:** The muzzle gas glows for about 5 ticks. Air stirred by walking, breathing,
  bullets or falling matter evens out its temperature quickly; still air (and convection, which
  makes no currents) is unchanged. Energy is still conserved exactly. The heat pass reads one
  more byte per cell (the cell's current); the benchmark is unchanged.

## D-035 — A gas cell holds a mixture of gases

- **Status:** Accepted (M6a). Supersedes D-005 for gases, and D-030 and D-032.
- **Context:** With one gas per cell, a cell has no concentration: rules had to count the cells
  around (diffusion read up to 800 of them, bulk buoyancy 450), every tick, for every awake cell,
  and a mixture never looked settled cell by cell. Shooting into a 1000 × 1000 room of air, or a
  big ball of CO2, dropped the game to 10 frames per second. The user also wants realistic mixing
  (and later, mixed liquids: ONI's separate "water" and "dirty water" felt strange).
- **Decision:** A gas cell is `ELEM_GAS` and holds a mass of each species (O2, CO2, H2, water
  vapour, N2: `GasMix`, 20 bytes). Diffusion trades each species down its mole-fraction difference
  with a neighbour (Fick, no statistics); buoyancy compares real densities (at 1 atm) of column
  windows, 1 to 16 rows, and lifts or shifts columns; heavy gas spreads by comparing column
  weights to the side; pressure evens out with jumps of up to 16 cells; reactions happen in the
  cell; water vapour condenses at its dew point. Cells sleep when they match their neighbours
  within tolerances (and exactly-equal cells cost a few loads). Painted air is one uniform
  mixture, settled at once. The species keep their element IDs as names and property rows.
- **Alternatives:** keep one gas per cell and tune it (3–5× faster at best, the statistical
  machinery stays); a coarser gas grid (blocks of 4 × 4 cells), against "every pixel is a cell".
- **Consequences:** Air, mixtures and their pressures are exact and mix smoothly; humidity exists;
  mixtures never unmix; hydrogen burns where it has mixed with air. Gas memory: 20 bytes per cell
  for the mixture and 4 for its cached density (190 MB at 4096 × 2048). Measured on 4 threads in a
  1000 × 1000 room: shooting 1.2–1.4× faster, a big CO2 ball 1.6–2× faster, the default level
  settles at once; a soup of random single cells of pure gases (`pr-bench`) is 20–25% slower
  (every cell differs from its neighbours), and gas expanding into a world of vacuum (the demo)
  2.5× slower. Not the 4–5× hoped for: most visited cells have nothing to do (first put down to
  chunk-wide dirty rects; tracking per tile later showed they are mostly the neighbours of active
  cells, D-036), and heat keeps pressure moving (ROADMAP M6c). Pressure precision is 0.3%
  between neighbours and 0.2% per 16 cells (a finer one made shooting costlier). The model can be
  reused for liquids carrying dissolved or suspended matter (ROADMAP).

## D-036 — Gas computed as a field, with tile activity and (later) an air velocity

- **Status:** Proposed (2026-11-23); part (3) prototyped per cell face and integrated, then
  reverted on 2026-11-24 (too slow under steady heating; PERFORMANCE → step 3). Part (1)
  implemented 2026-11-24 for the movement and gas density passes, with a smaller gain than estimated (about 5%, see PERFORMANCE → Benchmark log).
  Part (2) on hold (2026-11-24): measured in the game, one field step over the awake tiles of the
  busy cases costs about as much as today's gas and heat rules together on today's data layout
  (55 ns per cell, 204 000 cells when firing), before buoyancy and long-range pressure; and
  exchange-based buoyancy is diffusive, so it needs part (3). The revised order is in
  `docs/PERFORMANCE.md` → Proposed next steps → Order.
- **Context:** After D-035 a gas cell stores a mixture, but gas still moves mostly as cells trading
  places at random, one direction per tick: 40 ns per visit for a cell that has nothing to do,
  100 ns and more for an active one, and three visits in four find nothing to do when firing (a
  chunk wakes one bounding rectangle). D-035 made the play-tested cases 1.2–2× faster, not enough
  to keep a busy room at 60 ticks per second.
- **Proposal:** (1) Track activity per 16 × 16 tile. (2) Gas cells stop moving: each tick, the gas
  and heat crossing every face between two cells are computed from the previous tick's values and
  applied at once (as the heat pass does), one array per species, no randomness, no checkerboard
  for gas; pressure is also relaxed between coarse blocks, since neighbour exchange alone moves it
  one cell per tick. (3) Later, a wind velocity per small block, driven by pressure and buoyancy,
  carries the gas, with sound slowed to 10–20 m/s.
- **Evidence:** a stand-alone prototype of the exchange (`bench/proto_gas_field.c`) runs a fully
  active 1024 × 1024 grid at 28–31 ns per cell on one thread and 8.5 ms per tick on four (5.7 ms
  with AVX2), conserving mass and energy to 1e-9. It leaves out buoyancy, sleeping and other
  matter.
- **Alternatives:** keep tuning the swap rules (each fix so far bought 10–30%); a coarser gas grid
  for everything (against "every pixel is a cell"); a full incompressible fluid solver per cell
  (loses pressure differences between sealed rooms, and costs a global solve).
- **Risks:** thin walls under a coarse velocity grid (connectivity per tile needed); loops that
  stop vectorising on one of the three compilers; the gas tests rewritten again; gas loses its
  per-cell grain (to be drawn as noise). Estimates in this project have been 2–4× optimistic:
  measure each step in the game before removing what it replaces.

## D-037 — Cells stay 5 cm (10 cm considered and rejected)

- **Status:** Accepted (2026-11-24). No code change: `CELL_SIZE_M` stays 0.05.
- **Context:** Gas is too slow for a busy scene (PERFORMANCE → Proposed next steps), and going to
  10 cm cells was considered as the cheapest speed-up: the same scene in metres would hold 4×
  fewer cells (120 000 in view at zoom 3 instead of 480 000 at zoom 1.5), for about 4× less work,
  estimated from cell counts and never measured. It was decided for a day, then withdrawn by the
  user: the game stays at 5 cm and the simulation is made fast enough at that scale.
- **Decision:** One cell is a 5 cm cube. Performance comes from the gas rework (D-036), activity
  tracking per tile, and simulating only near the player (ROADMAP M6b, M6c).
- **Why not 10 cm:** coarser detail everywhere: the thinnest wall, a bullet, a casing and a drop
  of water would be 10 cm, the player 18–19 × 4–5 cells instead of 37 × 9, and each cell 3 × 3
  screen pixels at the zoom that frames the scene as wanted. 15 cm was ruled out earlier for the
  same reason (a 12 × 3 cell player).
- **Consequences:** The performance target must be met at 5 cm: about 1.1 million cells in view
  at zoom 1. If the gas rework falls short, the levers left are gas at 30 Hz and, last, a coarser
  gas grid or a larger cell; both would reopen this decision. About 20 constants are written in
  cells (body sizes, step height, rifle length, breathing volumes, fall speeds), so the cell size
  is not a one-line change.

## D-038 — Bodies drawn at half size, zoom ×2

- **Status:** Accepted (2026-11-24), asked for by the user after play-testing.
- **Context:** With a 1.85 m player (37 × 9 cells) at zoom ×1, the view showed little of the world
  around the player. Making cells bigger (10 cm) was rejected (D-037): it coarsens every material.
- **Decision:** Bodies are half a person's size: the player and other creatures are 5 × 17 cells
  (25 cm × 85 cm), and the default zoom is ×2. The world keeps its scale (5 cm cells): gases,
  liquids, particles, heat and bullets are unchanged. What is proportional to the body is halved:
  the shoulder (row 4), the rifle (9 cells), the mouth (row 2), the ledge a walker steps up
  (`STEP_UP` 2) and the piles it kicks (`KICK_ROWS` 4, `KICK_AHEAD` 6). What stays a person's:
  breathing (volumes and rates), body heat (up to 100 W), running speed (4 m/s) and jump height
  (50 cm).
- **Consequences:** a body loses less heat (less surface: 28 W in the 0 °C test room),
  floats with less of it out of the water (it treads water, its mouth out about three quarters of
  the time), and stands in water up to about 40 cm. Gameplay and the view decide sizes from here
  on, not a human's measurements.

## D-039 — Buoyancy moves by layers: rising and sinking, crossing whole layers

- **Status:** Accepted (2026-11-24), after the user asked whether hot air should rise (it barely
  did) and reported lava pools slowing the game down.
- **Context:** Gas rose by one rule only: a light cell lifted up to 16 rows into denser gas above
  it (D-030). A heavy blob sank as a whole that way (the light gas under it was lifted through
  it), but a warm blob, the mirror case, did not rise: its top cells were lifted one by one into
  the cold air as thin streaks that cooled in place, and the rest stayed where it was. A 1.2 m
  bubble at +107 K moved its centre 1 m in 5 s; a real one rises at about 1.3 m/s.
- **Decision:** A heavy cell can also sink through lighter gas below, with the same window drive.
  A move crosses the layer ahead to its far side if that layer ends within `MAX_MOVE` rows; if
  the layer ahead is thick but the cell's own layer is thin, the cell just ahead crosses the
  cell's layer instead (at the cell's drive); only when both layers are thick does a cell dive
  into the other gas (a finger). A stable vertical density difference (lighter gas above) is no
  longer a reason for a gas cell to stay awake: cells kept awake for it were thrown off a warm
  blob's edges as stray streaks. (It does not make warm air over cold sleep: that stays awake
  while heat conducts across the boundary and pressure follows.)
- **Consequences:** the bubble rises at about 1.5 m/s and reaches the ceiling of a 12 m room in
  4 s; CO₂ over oxygen turns over cleanly (mean rows 44.7 and 18.3 in the test, against 37.3 and
  25.7). A big CO₂ cloud in air mixes a little more as it sinks (fingers both ways): about 15%
  more work in that case. Lava pools are not fixed by this: their cost comes from pressure (see
  PERFORMANCE → Proposed next steps).

## D-040 — Only the area around the player is simulated

- **Status:** Accepted (2026-11-24), asked for by the user.
- **Context:** The world is 4096 × 2048 cells, and every awake cell costs the same wherever it is:
  a lava pool or a settling gas cloud the player has left keeps the frame rate down. Attempts to
  make continuous heat sources cheap (PERFORMANCE → Proposed next steps) did not succeed.
- **Decision:** The world is cut into regions of 512 × 512 cells (8 × 8 chunks, `SIM_REGION`). Only
  the 3 × 3 regions around the player are simulated (1536 × 1536 cells); outside, time stops, as
  in Noita. Frozen chunks keep their pending wakes; heat does not cross into them; water is not
  taken from them; creatures there are not updated; free particles fly on and land there. The
  user proposed 500 × 500; 512 keeps regions on chunk boundaries. Fixed regions (not a window
  centred on the player) change only when the player crosses a region border, and always leave
  512 to 1024 simulated cells around the player, more than the view shows at zoom ×2.
- **Consequences:** The cost no longer depends on the world's size: lava far from the player went
  from 65.7 to 2.5 ms per tick. What is near the player costs as before (lava pools beside it
  still grow within the area). Zoomed out (×1 or less), frozen parts of the world can be in view.
  Things left behind do not evolve while away (a fire does not burn out, water does not drain).
  Saving and loading regions (world streaming) is later work (M6).

## D-041 — The level is a maze; the player has a jetpack

- **Status:** Accepted (2026-11-24), asked for by the user.
- **Decision:** `scene_level()` builds a perfect maze (depth-first) over a grid of rooms of
  `MAZE_ROOM_W` × `MAZE_ROOM_H` = 488 × 51 cells (about a screen wide at zoom ×2; three player
  heights tall) separated by `MAZE_WALL` = 20 cells of dirt (boulders of rock inside it). A link is
  a doorway through a side wall (as tall as the room) or a shaft through a floor (as wide as the
  room is tall), so both openings are 20 × 51 cells. 8 × 28 rooms in the 4096 × 2048 world; the
  player starts in a room near the middle. The jetpack (infinite, no fuel yet) fires while the jump
  key stays held for 12 ticks, so a tap is still a jump (ELEMENTS → Deviations 17).
- **Consequences:** The level is all air, so the player digs and shoots through walls to shortcut
  the maze. Rooms are narrower than the simulated area (D-040), which holds about 9 rooms.
  Jetpack exhaust pushing on the air, fuel and heat are later work.

## D-042 — Hit points, monsters, blood and crates

- **Status:** Accepted (2026-11-24), asked for by the user.
- **Decision:** A creature has hit points (`hp`, 100 for the player), separate from `health`, the
  oxygen state. Bullets that reach a body cell take 12 hp and stop (the player's own bullets do
  not hurt it). A hit and a dead body's bleeding throw droplets of a new liquid, `ELEM_BLOOD`, as
  free particles: blood is real matter that pools and flows, not a decoration. Monsters are
  creatures (a 6 × 6 walker and an 11 × 11 flyer, drawn as boxes over their body cells) with
  `enemy_think()` setting their movement before `physics()`; they do not breathe and give off no
  heat. Crates are not cells: `Pickup` boxes in `World.pickups`, falling while open below, picked
  up by overlap. Rifle ammunition is counted only for a `Rifle` made with `rifle_init()` (a zero
  `Rifle` is endless, which keeps the weapon tests as they were). The restart key moves from `R`
  (now reload) to `F5`.
- **Pathfinding:** see D-043.
- **Chase rule:** a monster notices the player when it sees it on the screen and chases until
  the player is off the screen (`World.view_w/view_h`, in cells, set by the game each tick), even
  with a wall between: there is no pathfinding yet (a search over the maze's rooms is the cheap
  next step), so it pushes against the wall.
- **Consequences:** Monster bodies start at room temperature, because a 310 K body in 293 K air
  kept hundreds of chunks in the heat pass (50 monsters: 12 ms a tick against 2 ms). Monsters
  cost little while frozen outside the simulated area (D-040). Monsters only bite; ranged enemies,
  knockback, the player's death screen beyond a message, and sprites are later work.

## D-043 — Monster pathfinding: distance fields over blocks around the player

- **Status:** Accepted (2026-11-24), asked for by the user.
- **Context:** Monsters chase the player until it is off the screen (D-042), but without a route
  they pressed against walls. The maze is destructible, so a route over its room graph would go
  stale; a search over cells is far too big.
- **Decision:** One shared distance field per kind, from the player, over blocks of 6 cells in a
  window of twice the screen around it (`path.c`), rebuilt every 20 ticks when an alert monster
  without line of sight needs it. A monster follows the descent of the field (one search serves
  every monster, whatever their number). Flyers use blocks with all neighbours open (they are 11
  cells wide); walkers use the moves a walker can make (side, down, up one block), so a player
  above them in a shaft is out of reach and they fall back to walking at it. In sight they still
  go straight at the player.
- **Consequences:** Two searches over about 36 000 blocks every 20 ticks while a monster hunts
  without sight (well under a millisecond). Walls under 6 cells thick can be missed by the
  sampling, and a monster outside the window (twice the screen) has no route. Routes follow
  digging within 20 ticks. A smarter walker (jumping up shafts with the jetpack-less jump is
  impossible anyway) and monsters that open doors are not needed yet.
