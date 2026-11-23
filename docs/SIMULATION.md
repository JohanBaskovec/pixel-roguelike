# Simulation

How matter behaves. Code lives in `src/sim/`. Every rule here needs a behaviour test in `tests/`.

## Units and scale

- **SI units internally.** Temperatures are in **Kelvin** (shown as °C in the UI), density in
  kg/m³, heat capacity in J/(kg·K), conductivity in W/(m·K).
- **Physical cell size (working assumption): 1 px = 5 cm**, and each cell is treated as a cube
  (`CELL_SIZE_M = 0.05`, volume 1.25 × 10⁻⁴ m³). A water cell therefore weighs 0.125 kg. This is an
  open design question (see `VISION.md`) and lives in one constant.
- **Gravity** is `g = 9.81 m/s²`. At 60 ticks/s and 5 cm/px that is ≈ 0.0545 px/tick².
- **Time:** one tick = 1/60 s of game time for movement. Heat uses an explicit time-scale factor
  (see below).

## Behaviours (phases)

Each element has exactly one behaviour, used for dispatch in the movement pass:

| Behaviour | Moves? | Examples | Milestone |
|---|---|---|---|
| `NONE` | never; it is "nothing" | void | M1 |
| `STATIC` | never moves on its own; changes only through player edits, phase changes or reactions | dirt, rock, iron, ice | M1 |
| `LIQUID` | falls, flows diagonally, spreads sideways | water | M1 |
| `GAS` | drifts, diffuses, stratifies by density | oxygen, CO2, hydrogen, steam | M1 (steam M3) |
| `POWDER` | falls, slides diagonally, piles up; no sideways spread | sand, rubble | M5 |

**Void** is vacuum: no mass, no heat capacity, no conduction. It is what a cell becomes when
matter leaves it.

## Movement pass

### Scan order, phases and the tick stamp

- Chunks are processed in the 4-phase checkerboard (`ARCHITECTURE.md`). The phase that goes
  first rotates every tick, so no chunk group always gets first pick of free space at its borders.
- Within each chunk's dirty rect, rows are processed **bottom to top**. Within a row, the
  horizontal direction **alternates every tick**, so nothing drifts systematically to one side.
- Each cell has a `stamp` byte: the low 8 bits of the tick in which it was last updated or
  moved. A cell whose stamp equals the current tick's stamp is skipped. This stops a rising gas,
  which moves into a row that has not been processed yet, from being updated twice in one tick.
  A long-sleeping cell can, by chance (1 in 256), carry the current stamp when it wakes; it then
  waits one extra tick. That is harmless and deterministic. Painted cells get a stamp that
  guarantees they are simulated in the next tick.
- `STATIC` and `NONE` cells are skipped with a single table lookup.

### Reach: rules may read and write only within `MAX_MOVE` (32 cells)

This is the threading invariant from `ARCHITECTURE.md`, and it applies to **reads** as well as
moves: lookaheads are capped at 32 cells. A multi-cell move is done as **one swap** from start to
destination. The cells passed over are void (left unchanged) or a lighter fluid that trades
places with the mover, so the result looks the same and costs one swap instead of many.

### The "can displace" test (density swaps)

A moving cell **A** may move into target cell **B**, going down or sideways, if:

1. B is **void** → move (B's position becomes void), or
2. B is a **fluid** (liquid or gas) that is **lighter** than A → **swap** A and B, together with
   their temperatures (and masses from M4).

Moving **up** is the gas buoyancy rule below. Static cells are never displaced. Cells beyond the
world edge read as the pseudo element `ELEM_OUT`, which behaves as static.

**Diagonal moves may not squeeze between two blocking cells.** A diagonal step is refused when
both orthogonal neighbours it passes between are static. Otherwise matter leaks through
1-pixel-thin diagonal walls.

Density comparisons use a pre-quantised integer table (`g_elem_density_q`, g/m³). From M3, gas
buoyancy uses temperature-adjusted density (see Heat → Convection).

### Liquids

```
update_liquid(x, y):
    1. fall        if it can displace the cell below: fall, accelerating under gravity;
                   if the cell below is its own liquid, still falling, with open space further
                   down: wait in mid-air
    (the cell is now still: it landed or was resting)
    2. pressure    if the top surface of its body of still water is at least one row above an
                   open cell above or beside it: request a transfer to that cell (see Liquid
                   pressure) and stop
    3. diagonal    slide down-left/down-right, random order
    pressured = (the cell above is still liquid of the same kind)
    4. sideways    (surface cells only) flow toward a drop (a cell it could fall into) seen within
                   LIQUID_LOOKAHEAD (32) cells, looking through its own liquid; up to SPREAD cells
                   through open space, or straight to the drop when it looked past its own liquid;
                   current flow direction first (aux bit 7), then the other
    5. settle      do not wake: the cell sleeps (unless it is not yet part of a labelled body and
                   has open space beside it: then look again next tick)
```

Why each part exists:

- **Fall speed.** `aux` bits 0–6 hold a fall speed in 1/16 px/tick. It grows by 1 with
  probability 0.87 per tick, which is ≈ 9.8 m/s² at 5 cm/px. The cell falls `1 + speed/16` cells
  per tick, up to ~8 px/tick. A falling cell always has a speed of at least 1, and the speed is
  reset on landing, so "speed 0" means **still**. On landing, the speed is added to the sideways
  spread for that tick (a splash).
- **Still vs falling.** Only still liquid forms bodies and carries pressure. Falling water (a
  stream pouring into a pool) is not part of the pool, and water on top of a still cell only
  counts as "covering" it if it is still too.
- **Pressure** does all the work of levelling and of pushing water out of openings: see below.
  A pressured cell (still water on top of it) does not flow on its own.
- **Drop rule.** Surface cells flow only toward somewhere lower, so a level surface with a
  partial top row is still and sleeps.
- **Looking through its own liquid.** Otherwise only the edge cell of each step on a slope could
  move (its neighbours block each other), and poured water piled into a stepped mound faster than
  it could flow away. With it, the whole top row of a step flows down at once.
- **Waiting behind a falling stream.** Where the chunk below is scanned after this one, the
  leading cells of a falling stream have not moved yet when the cells behind them look down. If
  those cells "landed" on them, they would spread sideways, and the stream bulged below every
  chunk border (every 64 px). They wait instead, keeping their speed. Test:
  `test_falling_stream_has_no_chunk_bulges`. They wait only if the falling column below them
  ends in open space. If it ends on something that does not move, it is a real jam, and they
  land as usual; otherwise a stream pouring into a pool would stack up into a tower.
- **Falling streams break into spray.** Each falling cell gains speed at random (87% chance per
  tick), so a long stream spreads into droplets. That matches real streams falling many metres
  (Plateau–Rayleigh break-up), and the stream is coherent near its source. Deterministic gravity
  was tried and rejected: cells released together then fell as rigid horizontal lines.

### Liquid pressure (M2b; D-018)

Code: `src/sim/pressure.c`.

**Bodies.** Still liquid is grouped into connected bodies (4-connected, same element). Each body
knows its **top**: the highest row holding one of its free-surface cells (a still cell with void,
a lighter fluid or falling water above it). A body with no free surface (a sealed container) has
no top and no pressure.

**The rule.** A still cell P next to an open cell Q (void or lighter fluid; above P or beside it)
asks for a transfer if its body's top is at least one row above Q. After the phase, transfers
are carried out serially: a free-surface cell is taken from the body's top row (or, once that
row is used up this tick, from the next rows down, as long as they are still above Q) and moved
to Q. So:

- water appears at the opening it is pushed toward, and the surface drops where the water really
  came from, which is the physically right bookkeeping for an incompressible liquid;
- pools level to within one cell over **any distance** (a one-cell step never qualifies, so a
  surface with a partial top row is stable, and settled pools sleep completely);
- U-tubes and communicating vessels balance; a reservoir drains through an opening in its wall
  until both sides are level; a heap or a column of water spreads out.

**How bodies are found, every tick, exactly:**
1. Chunks where still liquid changed (a still cell moved or started to fall, a falling cell
   landed, an edit, a transfer) are relabelled: a flood fill gives each connected piece within the
   chunk an id (up to 255 per chunk; further pieces get no pressure until relabelled) and records
   its highest free-surface row. In parallel, one job per chunk.
2. A union-find over all pieces joins pieces that touch across chunk borders (a cheap pass over
   chunk borders; a few thousand nodes). Each root records the body's top and the list of pieces
   that reach it. This is serial and deterministic.
3. Chunks whose body top rose get all their cells simulated this tick, since cells next to
   openings may now be pushed.

Nothing is carried over between ticks except the labels of unchanged chunks, so there are no
stale values, and a change anywhere in a body is seen everywhere in it in the next tick.

**Earlier design, abandoned.** The first implementation (notes in D-018) propagated a per-cell
hydraulic-head field between neighbouring chunks. Values left over from a surface that had
dropped bounced between chunks and decayed only slowly; a small propagation cost made them linger
for many seconds, a large one made big pools imprecise. Labelling bodies exactly avoids the
problem altogether, and is also cheaper (no gradient traces).

**Known limitations** (and where they are planned in ROADMAP.md):
- The flow rate through an opening is not physical: it is limited by how many cells request
  transfers per tick (up to 128 per chunk), not proportional to the square root of the depth.
  Planned in M4b (free particles, Torricelli outflow, jets).
- Different liquids do not push each other (only one liquid exists so far). Planned in M5.
- A body's surface pressure is taken as zero: trapped or pressurised gas does not push on water,
  and heating does not pressurise sealed containers. Planned in M4 (gas pressure acting on
  liquids). Note that a body completely full of liquid in rigid walls has nothing to push against
  in reality either; pipes connected to an open tank already work (they are one body).
- Transfers move a cell from the top of the body to the opening in one step. Visually this reads
  as the surface lowering and the water flowing out, but water does not travel through the body.

### Gases (M1 particle model)

One gas particle per pixel. A gas cell tries the following, in order:

1. **Buoyancy against other fluids.** If the cell above is a fluid that is denser than this gas →
   swap with probability `p_buoy = K_BUOY * (ρ_above - ρ_self) / (ρ_above + ρ_self)`, from the
   precomputed table `g_buoy_p`.
2. **Random walk.** Pick one of the 8 neighbours using the chunk RNG.
   - If it is **void** → keep stepping in that direction through void for up to `SPREAD` cells
     (per gas; lighter gases spread further), then move there in one swap. This is expansion.
     There is **no gravity bias against void**: with one particle per pixel, a gas that "fell"
     into vacuum would look like powder. Physically this is right too: over the height of a room,
     gravity barely changes a gas's density.
   - If it is **another gas** → swap with mixing probability `P_GAS_MIX`, or `P_GAS_MIX / 4` when
     the swap would move the denser gas up. This gives partial mixing on top of stratification.
   - If it is the same gas → nothing.
3. **Did not move.** Stay awake only if a move is still possible: a neighbour is void or a
   different gas, or a denser fluid sits above. A region filled with a single gas sleeps.

Gases never displace liquids sideways or downward; liquids sink through gas by their own update.

Expected emergent results: CO2 (1.98 kg/m³) pools under O2 (1.43 kg/m³), H2 (0.09 kg/m³) rises
above both, and water rains down through all of them. Gas painted into vacuum spreads out as a
thinning cloud of particles and never "fills" the box. That is the particle-model limitation
that M4 removes. All constants live at the top of `src/sim/sim.c` and in `src/sim/elements.h`.
Their current values are in `ELEMENTS.md`.

### Gases from M4: mass and pressure

Gas cells will carry `mass` (kg). Pressure follows the ideal gas law,
`P = (m / M) · R · T / V_cell`, where M is the molar mass. Each tick, gas flows between
neighbouring cells of the **same** gas in proportion to the pressure difference. Gas expands into
void by splitting off mass. A cell whose mass drops below a minimum disappears into void.
Different gases still never share a cell; they swap by density as above. Rendering alpha scales
with mass. This is the ONI model on a pixel grid; see `DECISIONS.md` D-005.

## Heat pass (M3)

Temperature exists on every non-void cell from M1, and starts at 293.15 K. Heat starts flowing in
M3.

### Conduction

Each cell has a thermal mass `C_i = ρ_i · V_cell · c_i` (J/K). Between two neighbours i and j
(4-neighbourhood, sharing a face of area `dx²` at distance `dx`):

```
k_eff  = 2·k_i·k_j / (k_i + k_j)          // harmonic mean: two half-cells in series
G_ij   = k_eff · dx                       // conductance, W/K  (= k_eff · dx² / dx)
Q_ij   = G_ij · (T_j − T_i) · dt · HEAT_TIME_SCALE
T_i'   = T_i + Σ_j Q_ij / C_i
```

- **Void has k = 0.** Vacuum insulates perfectly; there is no heat radiation (D-017).
- **Jacobi double-buffering.** Read `temp[cur]` and write `temp[next]`. Every cell computes the
  same `Q_ij` for a given pair (a symmetric function of i and j), so **energy is conserved** and
  the result does not depend on thread scheduling.
- **Stability.** The explicit scheme is unstable when `Σ_j G_ij·dt·S / C_i` gets close to 1. The
  stiff case is a light gas cell (tiny C) touching iron (huge k). Each pair's flow is limited so it
  can never push the two cells past their common equilibrium:
  `|Q_ij| ≤ |T_j − T_i| · (C_i·C_j / (C_i + C_j)) / 4`. The divisor 4 is the neighbour count. The
  limiter is symmetric, so energy is still conserved.
- **Sleeping.** A chunk is heat-active while any neighbouring pair differs by more than
  `HEAT_EPSILON_K`, or while something moved in it. Other chunks skip the heat pass. The heat pass
  may also run every N ticks with a proportionally larger `dt`, within the stability limit.

### Conduction alone is slow; convection does most of the work

**Pure conduction** spreads heat about `√(α·t)`. In still water (α ≈ 1.4 × 10⁻⁷ m²/s) that is
≈ 3 mm in 1 minute, ≈ 1 cm in 10 minutes, and ≈ 2.3 cm in an hour. The time scale for a whole 5 cm
cell to equalise (`dx²/α`) is a few hours. Through iron (α ≈ 2.3 × 10⁻⁵ m²/s) it is about 2 minutes.
These values are correct. They match, for example, lakes that keep a warm surface layer over cold
water for months.

They do **not** match everyday experience. A pot heats in minutes, and an ice cube melts quickly in
a drink, because of **convection**: warm fluid gets lighter, rises, and mixes. In a falling-sand
simulation, settled liquid does not move, so without explicit convection a pool would conduct heat
like a solid and feel absurdly slow. **Convection must therefore be simulated** (see below). It is
not something to fake by cranking up conduction.

After convection is in place, `HEAT_TIME_SCALE` (a single global, tuned in M3) may still be needed
to make conduction through solids playable. It speeds up all conduction equally. **Relative
behaviour stays realistic** (heat spreads through iron ~160× faster than through water); only the
absolute pace changes. Like every other deviation, it is documented in `ELEMENTS.md`. Tune it
*after* convection works, because the needed value will be much smaller.

### Convection

- Moving cells carry their temperature, so heat is advected by flowing water and gas.
- **Gases:** density depends on temperature, `ρ(T) = ρ_ref · T_ref / T` (ideal gas). Hot gas
  becomes buoyant, rises, cools and sinks. The existing gas buoyancy swaps produce convection
  currents once they use `ρ(T)`.
- **Liquids:** two cells of the *same* liquid stacked vertically swap with a probability that grows
  with their density difference, when the lower one is lighter (warmer). Liquid density uses a
  per-element thermal expansion coefficient. Water's is ≈ 2.1 × 10⁻⁴ /K at 20 °C, and water is
  **densest at 4 °C**. A small density curve for water in the element table gives realistic
  results for free: ponds freeze from the top, and the bottom water stays at 4 °C.
- **Sleeping:** a chunk with a vertical temperature inversion in a fluid (warm under cold, beyond
  `HEAT_EPSILON_K`) stays active. A stably stratified fluid sleeps.
- **Fallback, only if profiling forces it:** give fluids an effective conductivity
  `k_eff = Nu · k` (Nusselt-number multiplier), the standard engineering shortcut for convection.
  It is cheaper but has no visible flow. Record it in `DECISIONS.md` if used.

### Phase changes and latent heat

Each element may define a lower and an upper transition, each with a threshold temperature, a
target element, and a latent heat L (J/kg). For example, water has ice below 273.15 K (L = 334 kJ/kg)
and steam above 373.15 K (L = 2257 kJ/kg).

A cell beyond its threshold stores the excess energy as temperature in its current phase:
`E_excess = m · c · (T − T_thr)`. Once `E_excess ≥ m · L`, the cell becomes the target element at
`T = T_thr ± (E_excess − m·L) / (m · c_new)`. Energy is conserved, and a mixed region "holds" at the
melting or boiling point while it changes phase, as in reality. A small hysteresis band prevents
flicker.

Boiling points are at 1 atm until pressure exists (M4). After that they may depend on pressure.

## Randomness

- RNG: xorshift32 (`src/core/rng.h`), one per chunk job, reseeded at the start of every chunk
  update from `splitmix64(world.seed ^ splitmix64(tick) ^ (cx << 32 | cy))`.
- Player edits use their own generator, seeded from the world seed, the tick and the stroke
  position, so an edit stream replays identically.
- Use it for: random direction choices, swap probabilities, and the `shade` of new cells.
- Never use `rand()`. Never share an RNG between threads. Never let the outcome depend on thread
  count (see `ARCHITECTURE.md` → Determinism).

## Conservation checks (tests)

- Movement never creates or destroys cells: per-element counts stay constant without edits or
  phase changes.
- Without boundary losses, total thermal energy `Σ C_i·T_i` stays constant within float tolerance
  (M3).
- Gas mass is conserved (M4).
