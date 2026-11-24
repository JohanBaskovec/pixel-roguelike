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

## M2 – Performance foundation ✅

- [x] Thread pool (SDL threads): fork-join, one thread per logical core, the caller works too
      (`src/core/jobs.c`)
- [x] 4-phase checkerboard chunk scheduling, parallel per phase; reach of 32 px per job enforced
      (assert on every swap in Debug)
- [x] Dirty rects propagate across chunk borders (per-job wake buffers merged after each phase,
      D-019); chunks sleep and wake correctly
- [x] Deterministic per-chunk RNG; test that 1, 3 and 4 threads produce the same world
      (`test_same_result_for_any_thread_count`); ThreadSanitizer build (`PR_TSAN`) runs clean
- [x] Texture upload of dirty chunks only, and only those in view; one upload per chunk row;
      conversion on the thread pool (D-020)
- [x] HUD timers: simulation, drawing (convert + upload), whole frame; thread count; zoom
- [x] Default world **4096 × 2048** (8.4 M cells); camera pan (middle-drag, WASD/arrows), zoom
      (Ctrl+wheel, around the cursor), Home shows the whole world
- [x] `pr-bench --threads N --fill F`

Moved to later (not needed for M2):
- [ ] Test that spreading water behaves the same wherever chunk borders fall (D-014). Falling
      streams and pouring are covered already
- [ ] Experiment: a permanent 1-cell wall ring at the world edge to drop bounds checks from
      neighbour reads

**Acceptance:** 4096 × 2048 world, 60 ticks/s with about 2 M cells active, on a 6–8-core
desktop. Thread-count determinism test passes.

*Status:* the determinism test passes, and ThreadSanitizer is clean. The speed target is **not
yet verified on a desktop**. On the 4-vCPU, 2.1 GHz development VM, 2.3 M active cells take
42 ms/tick with 4 threads (3.7× the single-threaded speed), so about 24 ticks/s. A desktop core is
roughly 2–3× faster and a 6–8-core desktop has twice the threads, which should bring this to
about 10 ms/tick. Check with `pr-bench --width 4096 --height 2048 --fill 0.7`.

## M2b – Water pressure (D-018) ✅

- [x] Still liquid grouped into connected bodies, each with its top surface row: labelled per
      changed chunk (in parallel), joined across chunk borders by a union-find every tick. (The
      planned head field was built first and replaced; see D-018 amendment)
- [x] Water moves from a body's top to any opening at least one row below it (above or beside a
      still cell): pools level over any distance, U-tubes balance, reservoirs drain through
      openings, heaps spread. The random surface wander is gone
- [x] Settled pools sleep completely: no surface shimmer
- [x] Pressure view (`P`) and depth / kPa in the cursor inspector; HUD shows transfers per tick
- [x] Tests: `test_u_tube_balances` (within one cell), `test_poured_water_spreads` (whole basin
      level within one cell), `test_water_flows_through_hole_until_level`,
      `test_settled_world_sleeps` (a poured pool updates zero cells once settled)
- [x] Benchmark before/after recorded (no measurable change)
- [x] Demo scene (`--demo`) includes an iron U-tube

Not done (see SIMULATION.md → Liquid pressure → Known limitations): flow rate through openings
proportional to √depth ("a dam pushes harder"); pressure in sealed bodies; pressure between
different liquids.

## M3 – Temperature ✅

- [x] Heat conduction between neighbouring cells; energy-conserving (tested); stable with extreme
      contrasts (tested); 1000× speed-up for solids and liquids, 10× for gases (D-022)
- [x] Moving cells carry their temperature, so convection emerges from movement
- [x] Hot gas is less dense (ideal-gas scaling), so warm air rises (tested)
- [x] Liquid convection: warm liquid swaps upward through colder liquid of the same kind; water
      uses its real density curve (densest at 4 °C). Test: a pool heated from below warms at
      the top within 20 s
- [x] Phase changes with latent heat: water ↔ ice, water ↔ steam (new element: steam), via the
      stored/physical temperature (D-021). Tests: melting in warm water, all four transitions
- [x] Temperature view (`T`), Heat and Cool brushes (`H`, `C`), Steam tool (`5`)
- [x] Hot cells glow (blackbody tint)
- [x] Transfers carry temperature: test with a U-tube whose hot water flows into the cold side
- [x] Heat sleeps where temperatures are uniform, even when cells move (D-023);
      `pr-bench --temp-spread K` exercises it everywhere

Known limitations: boiling does not conserve mass until gas mass (M4); hot gas rises while
mixing rather than as a tight plume (see SIMULATION.md).

## M4 – Gas mass and pressure

- [x] Gas cells carry mass; gas expands into vacuum and thins out; pressure = f(mass, T)
- [x] Gases flow from high to low pressure; different gases still do not share a cell
- [x] **Nitrogen** (inert gas) so that air can be realistic: 78% N₂, 21% O₂ by volume. Until
      then "air" is pure oxygen, which makes breathing and suffocation unrealistic
- [x] **Gas pressure acts on liquids:** a liquid body's surface pressure is the gas pressure above
      it, not zero. Water compares (gas pressure + depth) at its surface with (gas pressure) at an
      opening. Gives pressurised tanks pushing water up pipes, and trapped air resisting water
      coming into a container. Test: sealed tank, pressurised gas on top, water rises in a pipe
      Done (D-025): tested with a pressurised tank, a barometer and trapped air. Total pressure
      also evens out across different gases (to ~3%).
- [x] **Pressure from heating sealed gas** (ideal gas law with M3 temperatures). Test: heated
      sealed container's gas pressure rises as P ∝ T
- [x] Gas pressure in the pressure overlay; cursor inspector shows partial pressures
- [x] Revisit D-005 and record the outcome (D-005, D-024)

## M4b – Flowing water (free-flying particles)

Fast-moving matter leaves the grid while it flies, as in Noita, and lands back into it.

- [x] Free particles: position and velocity (x and y) in floating point, drawn over the grid;
      created when a cell is launched, turned back into a cell where it lands. Deterministic
      (fixed update order), bounded count
- [x] **Outflow speed from depth** (Torricelli): water leaves an opening at √(2·g·depth), as a
      jet; the number of cells per opening cell per tick follows from the speed (about 1.5 at
      1 m depth, 5 at 10 m). Test: a deep reservoir drains faster than a shallow one, and the
      jet reaches further. Done for free outflow (measured within 5%); once the water outside
      reaches the opening, flow is again not limited by the opening's size (see SIMULATION.md)
- [x] Better streams, splashes and spray: falling water that hits something keeps part of its
      momentum sideways; drops and jets arc
- [x] Revisit the falling-stream rules (D-014) once particles carry falling water: falling
      streams stay on the grid (D-026)

## M4c – Creatures

Characters are entities, not cells: own position, body shape and physics. Depends on the open
question in VISION.md (colonists given orders, or a directly controlled character); this
milestone builds what both need.

- [x] Body: a pixel mask (a person is about 35 × 9 px at 5 cm per pixel), walking, stepping up
      small ledges, falling, collision with solids; updated on the main thread between ticks
      (deterministic, like edits)
- [x] **Bodies displace matter:** each tick the body is stamped into the grid; gas and liquid in
      the cells it moves into are pushed into the cells it leaves or to the sides, so moving
      creatures stir gas, push water aside and splash. Matter is conserved. Test: walking through
      a gas boundary mixes it; total cells per element unchanged
- [x] Buoyancy and drag in liquids; wading and swimming
- [x] **Breathing with real rates:** a breath moves ~0.5 L (4 cells) of air in and out, 12–16
      times a minute; at rest ~250 mL/min O₂ consumed (about 2 O₂ cells' worth per minute) and
      ~200 mL/min CO₂ produced, 5–10× more when working. With M4 mass, each breath removes and
      adds exact masses near the mouth. Optional global speed-up for gameplay (like
      `HEAT_TIME_SCALE`), documented as a deviation
- [x] Suffocation and CO₂ poisoning from the actual concentrations around the head (O₂ below
      ~16% impairs, below ~10% is dangerous; CO₂ above ~5% is dangerous). Test: a sealed room
      with one person reaches dangerous O₂ after the realistic time (about 2 h of game time for a
      5 × 3 m room)
- [x] Body heat (with M3): ~37 °C, ~100 W at rest, heating the cells around it
- [x] A simple test creature that wanders, for trying all of the above (key K)

Acceptance notes (M4c): the body is a 9 × 35 rectangle; matter it displaces moves to the cells it
leaves (exactly conserved; it does not splash). The sealed-room test gives 6.9 h, not 2 h: the
world is one cell (5 cm) deep, so a 6.3 × 3.1 m room holds only 1 m³ of air; the time follows
from the oxygen use within 4%. Breathing also stirs the air around a person (D-027).

## M5 – Matter variety and reactions

- [x] Powders (sand, rubble). Digging solids produces rubble or debris (Dig tool: dirt → loose
      soil, rock → rubble)
- [x] Reactions table, e.g. H2 + O2 + ignition → steam + heat (combustion); more as design asks
- [x] Melting rock or iron into liquids at high temperatures
- [x] More liquids (molten metal, lava, maybe oil) with real densities
- [x] Moving matter stirs the air: gas currents from walking bodies, breaths and falling matter
      (added after play-testing: the air never moved)
- [x] Gas mixtures stay mixed: bulk buoyancy between gases (layers, blobs and plumes sink or
      rise, a mixture does not unmix), heavy gas spreads over the floor, stirred gas mixes fast
      (added after play-testing: CO2 and oxygen never mixed; D-030)
- [x] **Liquids push each other:** water pressure compares pressures instead of levels; a body's
      effective surface includes the weight of the lighter liquids stacked on it (their height ×
      their density ÷ its density), and openings get the same correction. Test: a U-tube with oil
      in one arm shows the real level difference (a manometer)

## M6a – Action roguelike: the player and the rifle

The game is an action roguelike (decided 2026-11-23; docs/VISION.md, D-031).

- [x] Zoom ×1 by default (zooming in and out still works); the camera follows the player (`F`)
      (×2 since D-038)
- [x] A player character, 1.85 m tall (37 × 9 cells; 17 × 5 since D-038): runs at 4 m/s with `A`/`D`, jumps 50 cm
      with `W` (physical keys: `Q`/`D`/`Z` on AZERTY), faces the mouse; breathes like any creature
- [x] An automatic rifle (the `Rifle` tool, default; left button fires, 720 rounds/min): a real
      bullet (one pixel of lead) flies at 900 m/s, stirs the air along its path, digs through
      dirt (about 30 cm) and chips rock, throwing the debris out of the hole, then falls and lies
      on the ground. Each shot ejects a casing (one pixel of brass): they pile up
- [x] No pixel is destroyed: dug cells become loose soil or rubble (debris), and stay in the world
- [x] Small clumps (20 cells or less, held by nothing) left by digging or shooting break loose
      and fall, so the player never gets stuck on a few floating pixels
- [x] A default level (a maze since M6b, D-041; before: hilly dirt with rock strata and boulders,
      air above); the Air brush paints evenly mixed air that is settled at once
- [x] M6b: the level is a maze of rooms 488 wide (about a screen at zoom ×2) and 51 tall (3 player
      heights), joined by doorways and shafts; an infinite jetpack (hold `W`, D-041)
- [x] The element tools stay, beside the rifle
- [x] Casings never block the player (after play-testing): bullets start at the muzzle, powders in
      flight pass through bodies, and walkers kick piles up to 40 cm high out of the way (20 cm for
      the half-size bodies of D-038)
- [x] No large clump of gas stays unmixed (after play-testing): diffusion down concentration
      gradients at several scales, at gentle indoor-turbulence rates, coming to rest once evenly
      mixed (D-032)
- [x] Powder in mid-air (falling soil, sand, rubble, casings, bullets, or lying on a body) never
      blocks the player; it falls through and lands beside it (after play-testing)
- [x] Air control: the player moves sideways during a jump
- [x] Water and lava flow around the body: landing in a pool raises the surface beside the body
      instead of putting the liquid over the head; jumping out leaves no air under the water
- [x] Every creature breathes in and out the air as it is mixed around its mouth (nitrogen,
      oxygen, CO2, anything else) through its lungs, and holds its breath under water (D-033)
- [x] Muzzle flash as matter: 1.6 g of propellant gas (CO2 and nitrogen at 1500 K) leaves the
      muzzle with each shot; it glows for a moment, and its current mixes it and its heat into
      the air (turbulent heat exchange, D-034)
- [x] Spent bullets melt into molten lead (327 °C), casings into molten brass (935 °C); molten
      metal solidifies into lead and brass
- [x] Gas mixtures (after play-testing: shooting into the air, or a big ball of CO2, dropped a
      1000 × 1000 room to 10 fps): a gas cell holds a mass of each gas, so air is one uniform
      mixture, gases diffuse and mix smoothly (Fick), mixtures never unmix, densities are real,
      water vapour condenses at its dew point (humid air), hydrogen burns where it has mixed with
      air (D-035). Those two cases run 1.2–2× faster; the game runs one tick per frame (slowing
      down a little) rather than collapsing its frame rate when a tick takes longer than real time
- Tests: digging and conservation, bullets stopping at iron and falling, air stirring, the same
  result on any thread count, small clumps, running and jumping, the Air brush, falling powder,
  air control, water around a body, breathing in and out and holding a breath, muzzle gas,
  melting, gas mixtures (diffusion, staying mixed, turning over, pooling, dew point)

## M6b – Roguelike, next

Asked for by the user, not designed yet (each needs the gas rework of M6c to run well in a busy
scene; the target scene is in `PERFORMANCE.md` → Budgets):
- [x] Bodies at half size (player 17 × 5 cells, 85 cm) and zoom ×2 by default, the world's scale
      unchanged (asked for after play-testing; D-038)
- [x] HP bar (100 hp), limited ammunition (30-round magazine, pack of 90, `R` reloads), ammunition
      and health crates (walk on them), two monsters that chase and bite the player (a 6 × 6
      walker and an 11 × 11 flyer), with hit points; bullets hurt them and scatter blood; dead
      bodies stay and bleed; all scattered over the maze (boxes, no sprites; D-042)
- [ ] Enemies that shoot at the player (creatures with a weapon and some behaviour), at the
      same half size
- [ ] Smoke grenades (smoke as one more gas species, or as fine particles carried by the air)
- [ ] Rockets (a blast: hot gas at high pressure, digging and debris)
- [ ] Acid that eats through walls (a liquid that reacts with solids)

- [x] Simulate only what is near the player: the 3 × 3 regions of 512 × 512 cells around it
      (1536 × 1536 cells); time stops outside (D-040). Lava pools left behind cost nothing (65.7 →
      2.5 ms per tick at 60 s); the world size no longer matters to the cost. World streaming
      (loading and saving regions) is M6
- [ ] Smooth zooming
- [ ] An HP bar with 100 HP (bullets, falls, suffocation)
- [ ] Heat and cold hurt: the further the air and matter around the body are from comfortable,
      the more damage per second (burns above about 45 °C at the skin, hypothermia below)

## M6c – Simulation, next

- [ ] Liquid mixtures (asked for: ONI's separate "water" and "dirty water" felt strange): a liquid
      cell carries what is dissolved or suspended in it (dirt, salt, pollution), the same way a gas
      cell carries its species; dirty water dilutes, mixes and settles. Miscible liquids mix,
      immiscible ones (oil and water) stay apart
- Faster and more credible gases (proposed, D-036; the plan, with measurements, is in
  `docs/PERFORMANCE.md` → Proposed next steps). In this order:
  - [x] Activity per 16 × 16 tile instead of one rectangle per chunk: movement and gas density
        passes (heat stays per chunk). Measured: 8–18% fewer cells visited, about 5% faster
        firing in a room, no change elsewhere; the awake region while firing is broad stirred
        air, not thin trails (PERFORMANCE → Benchmark log)
  - [x] Measure on the player's machine (Ryzen 7 4700U: firing at the sky for 10 s, Sim 50 ms).
        Cause: debris falling through clear air stirred and heated it; fixed (about 8 ms here)
  - [x] Thread scaling: costliest chunk jobs first, workers spinning briefly between batches
        (15–20% on 4 threads)
  - [x] Warm gas rises as a body, heavy gas sinks as one, layers turn over cleanly (D-039,
        after the user asked whether hot air should rise)
  - [ ] Continuous heat sources (lava pools) slow the game more and more: an artificial
        overpressure over the heat (35 kPa) spreads slowly, keeps a growing area awake and
        pushes the lava around. Needs pressure to even out fast (the velocity item below)
  - [ ] Gas at 30 Hz by tile parity (estimate about 1.3×); fewer wakes after an exchange
        (estimate about 10%)
  - [ ] *On hold:* gas as a field: every gas cell exchanges gas and heat with its four
        neighbours each tick, from the previous tick's values. Measured in the game: one such step
        over the awake tiles costs about as much as today's gas and heat rules (55 ns per cell,
        204 000 cells when firing), before buoyancy and long-range pressure; only with one array
        per species and together with a velocity (next item)
  - [ ] Air velocity per small block: wind, plumes, a rush of air when a sealed room opens,
        (per-cell faces prototyped, `bench/proto_air_velocity.c`; integrated and reverted: too slow
        in the lava case, see PERFORMANCE → step 3)
        pressure evening out in under a second (sound slowed to 10–20 m/s). Thin walls must stay
        airtight: prototype that first
  - [ ] Gas at 30 Hz; smaller gas storage (about 12 bytes per cell), if measurements ask for them
  - [ ] Free particles in parallel (binned by chunk) and pushed by the wind, when they become a
        cost
- [ ] Evaporation from water surfaces below 100 °C; water vapour in breath

## M6 – World

- [ ] Save and load (compressed chunks)
- [ ] Procedural world generation
- [ ] World streaming if worlds outgrow memory

## Later (depends on the design answers in VISION.md)

- Digging and building orders: a designation layer painted with the brush, then carried out by
  creatures
- Pathfinding through a pixel world (for colonists, if the design has them)
- Wall strength: containers and walls that crack or burst under pressure (water or heated gas)
- Audio, lighting, menus
