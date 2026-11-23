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
