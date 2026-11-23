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
| `POWDER` | falls, slides diagonally, piles up; no sideways spread; sinks slowly in liquids | sand, loose soil, rubble | M5 |

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
   their temperatures and masses.

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
  spread for that tick. Landing at `SPLASH_MIN_SPEED` (2 cells/tick, 6 m/s, a fall of about
  1.8 m) or faster, the cell **splashes** with probability 1/4: it leaves the grid as a free
  particle thrown sideways with 25–60% of its speed, and up to 25% of it upward (M4b).
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

### Powders (M5)

Sand, loose soil (dug dirt) and rubble (dug rock). A powder cell:

1. **Falls** if the cell below is void or gas, accelerating like a falling liquid (fall speed in
   `aux`), and moving up to `1 + speed/16` cells in one swap. It stops at the first liquid cell.
2. **Sinks through liquid** (a lighter liquid below it) one cell with probability
   `POWDER_SINK_P16` (about 0.15 m/s), without accelerating: grains settle slowly in water.
3. **Landed:** slides diagonally down if that cell and the one beside it are free (void or a
   lighter fluid) and it does not squeeze between two blocked cells. This piles powders at about
   45 degrees (tested: no column more than one cell above its neighbour).
4. Otherwise it is **at rest** and sleeps.

Liquids and gases cannot enter powder (no porosity), and creatures stand on it. **Digging** (the
Dig tool) loosens solids in place: dirt becomes loose soil, rock becomes rubble, keeping their
temperature; they then fall. Iron and ice are not dug. Spent bullets (lead) and casings (brass)
are powders too (M6a), so they pile up.

**Small clumps** (M6a; `sim_loosen_small_clumps` in `src/sim/edit.c`): after digging (the Dig
tool or a bullet), every clump of at most `CLUMP_MAX_CELLS` (20) dirt or rock cells, 4-connected,
next to the dug cells breaks loose (soil, rubble) and falls, so the player never gets stuck on a
few floating pixels. A clump that touches the world's edge, or holds on to iron or ice, stays; a
creature's body holds nothing up. Tested: a floating 16-cell clump falls, a 29-cell one stays, a
small one on an iron post stays.

### Liquid pressure (M2b, gas pressure M4; D-018, D-025)

Code: `src/sim/pressure.c`.

**Bodies.** Still liquid is grouped into connected bodies (4-connected, same element). Each body
knows its **top**: the highest row holding one of its free-surface cells (a still cell with void,
a lighter fluid or falling water above it). A body with no free surface (a sealed container) has
no top and no pressure.

**What presses on a liquid, as height.** Heights count what weighs on the liquid as a height of
it (`pressure_load_head`): the liquids stacked above (each cell counts its density over this
liquid's: 60 cells of oil on water count as 52; M5), plus the gas pressure over that column,
`P / (ρ·g·Δx)` (1 atm on water: 207 cells, 10.3 m). The column ends at a solid or powder, or at
3 empty cells in a row; shorter gaps are bubbles inside it. A free-surface cell's **head** is its
row minus the load above it, and the body's **top** is the smallest head among its free-surface
cells. An open cell's head is its row minus the load in and above it (void with nothing above:
its row). In the parallel request check the column is read only within reach (31 cells); the
transfer checks the whole column (up to 512 cells). In a world at one pressure (all vacuum, or all air at 1 atm) every
head shifts by the same amount and this is the plain row rule.

**The rule.** A still cell P next to an open cell Q (void or lighter fluid; above P or beside it)
asks for a transfer if its body's top is at least one cell above Q's head. After the phase, transfers
are carried out serially: a free-surface cell is taken from the body's top row (or, once that
row is used up this tick, from the next rows down, as long as they are still above Q) and moved
to Q. So:

- water appears at the opening it is pushed toward, and the surface drops where the water really
  came from, which is the physically right bookkeeping for an incompressible liquid;
- pools level to within one cell over **any distance** (a one-cell step never qualifies, so a
  surface with a partial top row is stable, and settled pools sleep completely);
- U-tubes and communicating vessels balance; a reservoir drains through an opening in its wall
  until both sides are level; a heap or a column of water spreads out;
- liquids push each other: water under a column of oil stands lower than open water by the
  oil's weight (a manometer: 60 cells of oil, 52 cells lower; tested);
- gas pushes on liquids: a sealed tank with pressurised gas drives water up a pipe until the
  column weighs as much as the gas pressure; air at 1 atm holds water about 207 cells up a tube
  sealed at the top (a barometer); water flowing into a sealed room of air compresses the air
  until its pressure balances the water (all tested).

**Jets (Torricelli, M4b).** A transfer through an opening **beside** P, where P is covered
(liquid or a wall above it: a hole below the surface, not the surface itself) and the cell beyond
the opening is open (or holds falling liquid), at a depth `d` (cells of head) whose outflow speed
`√(2·g·d)` is at least `JET_MIN_SPEED` (1 cell/tick, about 9 cells deep), launches the liquid as a
jet of free particles instead of placing it: about `speed` cells per tick (the volume a jet of
that speed carries through a one-cell opening; at most `JET_MAX_PER_REQUEST` = 8), each taken from
the body's top, starting staggered along the first tick's path, with ±3% of spread across the
jet. Measured: 150 cells deep, 3.95 cells/tick through a one-cell hole (theory 4.1); 25 deep,
1.58 (1.65); the deep jet reaches nearly twice as far. Openings at a free surface only level
(cell by cell): a surface does not spout.

**Moving a lighter liquid out of the way (M5).** Liquid entering a cell of a lighter liquid
lifts that liquid's column by one cell: the lighter cell moves to the first free cell above the
column, and that free cell's void or gas takes the source's place. With no free cell above, the
transfer does not happen. (Swapping the lighter liquid to the source would carry it to wherever
the source is, such as the other arm of a U-tube.)

**Moving gas out of the way.** When water moves into a gas cell Q, and the gas there is at about
the pressure of the gas above the source (within `GAS_SAME_REGION`, 5%: one region, e.g. one room
of air), the gas simply takes the source's place. Otherwise the gas at Q is **compressed** into
the cells of its kind around Q, and the gas above the source **expands** into the cell the water
left. Both are spread over up to `GAS_REGION_CELLS` (256) cells of that gas within
`GAS_REGION_RADIUS` (16), found by a breadth-first walk through gas (`world_gas_region`): one
cell of volume moved at a single spot would change its pressure by tens of percent, which is
207 × that many cells of water head, and the water would chase those spots in a cascade. Gas
equalises by diffusion, much slower than in reality, so such compressing transfers are limited to
one per body every `GAS_LIQUID_INTERVAL` (2) ticks: the rest of the gas keeps up, and a barometer
fills in about 20 s. Gas next to a liquid surface that changes (a transfer, heating) triggers a
relabel, so heating a sealed tank pushes its water too.

**How bodies are found, every tick, exactly:**
1. Chunks where still liquid changed (a still cell moved or started to fall, a falling cell
   landed, an edit, a transfer) are relabelled: a flood fill gives each connected piece within the
   chunk an id (up to 255 per chunk; further pieces get no pressure until relabelled) and records
   its smallest surface head (and that cell's row). Chunks are also relabelled where gas resting
   on liquid changed, or where heat changed fluid temperatures. In parallel, one job per chunk.
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
- Flow through an opening follows Torricelli only while the liquid leaves freely (a jet, below).
  Once the liquid outside reaches the opening, both sides are one body, and transfers move liquid
  from the body's top to any open cell of its surface, however narrow the passage between them:
  the flow is not limited by the size of the opening.
- Liquid entering a cell of a lighter liquid lifts that liquid's column by one cell (its cell goes
  to the first free cell above the column); with no free cell above, there is no transfer. The
  lighter liquid is never moved to the source, which may be far away.
- Gas pressure is resolved to one cell of liquid (490 Pa for water). Under mixed air, the oxygen
  and nitrogen cells' pressures can stay up to ~3% apart (see Gases), and a pool there may keep
  trading a few cells per tick between surface spots under one and the other.
- A gas bubble inside a liquid is just a lighter fluid that rises; its pressure does not follow
  its depth.
- Transfers move a cell from the top of the body to the opening in one step. Visually this reads
  as the surface lowering and the water flowing out, but water does not travel through the body.

### Gases (M4): mass and pressure

Each gas cell holds one gas with a **mass** (`World.mass`, kg). Its pressure follows the ideal gas
law, `P = (m / M) · R · T / V_cell`, with M the molar mass and T the physical temperature. A cell
painted with the brush holds the mass of 1 atm at its spawn temperature (0.15 g of air). Mass is
conserved by every rule below (tested to 1e-4).

A gas cell does the following each tick, in order:

1. **Buoyancy.** If the cell above is a denser fluid → swap with probability
   `p = K_BUOY · (ρ_above − ρ_self) / (ρ_above + ρ_self)`. Gas densities are taken **at 1 atm**
   for the cell's temperature (`elem_fluid_density`), whatever its actual pressure: a parcel at
   another pressure would be squeezed or expanded to its surroundings' pressure long before it rose
   or sank. So hot gas rises through cold gas, but thin gas does not shoot up. **Between two
   different gases** a single cell does not sink or rise by its kind: a warmer cell rises (a
   thermal, `K_BUOY · (T_self − T_above) / (T_self + T_above)`), and otherwise gas moves by **bulk
   buoyancy** (next section): CO2 sinks and H2 rises as layers, blobs and plumes, while a mixture
   stays mixed. **Oxygen and nitrogen count as air** (`AIR_DENSITY_0C`, the same density), so air
   never stratifies; real air needs kilometres of height to separate.
2. **One random direction** (4-neighbourhood) per tick. The neighbour there decides:
   - **Void:** a dense cell (at least `2 · GAS_MIN_FRAC` = 2% of its 1-atm mass) **splits**: half
     its mass goes 1..`SPREAD` cells into the void. Gas fills a vacuum and thins out. A thinner
     cell moves whole, as a particle (up to `SPREAD` cells), so a little gas in a big vacuum is a
     few drifting particles, not ever-thinner cells.
   - **Any gas at less than 1/`GAS_PUSH_RATIO` (1/2.1) of this pressure:** **push**. The
     neighbour's gas moves on into its own neighbour (straight on, else either side) where that is
     the same gas (it merges) or void, and this cell expands into the freed cell, half its mass
     each. Below a ratio of 2 the push would leave both halves below the gas it squeezed, which
     would push back next tick. Boiling water (one steam cell at ~1700 atm) clears the air this
     way, and suddenly heated gas pushes cold gas aside instead of mixing its heat into it.
   - **The same gas,** both thin (sum under 2% of 1 atm): the two **merge**.
   - **A different gas more than `GAS_BALANCE_TOL` (1%) below this pressure:** with probability
     1/16, **balance**. Its cell changes over to this gas: its gas is squeezed into the cells of
     its kind around, and the cell is refilled from the cells of this gas around (each region up to
     64 cells within 8, `world_gas_region`), so each region's pressure moves by a percent or two.
     Judged by the two regions' mean pressures, and only if this gas is still at least the other's
     afterwards (no flipping back). This is how total pressure evens out across different gases
     below the push ratio. Cells stay awake for it above a 3% difference.
   - **Else, a different gas:** the two **mix** (trade places) only if that lowers the number of
     same-gas contacts around the two cells. Gases spread into each other at their interface and
     then stop: the number of same-gas contacts only ever drops, so a mixed region goes to sleep
     instead of shuffling forever. (Mixing never moves the denser gas down on its own account:
     bulk buoyancy does that.) In still gas this is slow (`P_GAS_MIX`, 1.6% a tick): it
     stands for molecular diffusion, which covers about `√(2Dt)` = 4.5 cm (one cell) in 50 s, sped
     up about 40× (a deliberate deviation: real air is never perfectly still). Where the gas has
     a current (a cell or its neighbour), it is fast (`P_GAS_MIX_STIRRED`, 50%): turbulence.
   - Then, for any gas there: **pressure equalisation.** Walk a random 1..`GAS_REACH` (16) cells
     in that direction through gas of any kind, to the last cell of the same gas. If its
     temperature is within `GAS_JUMP_MAX_DT` (2 K), trade mass with it, otherwise with the
     adjacent cell (if it is the same gas). The amount leaves both at the same pressure:
     `dm = (m·T − m'·T') / (2·T_sender)`, because the moved gas keeps the sender's temperature
     and its heat mixes into the receiver (energy-conserving). Pressures within `GAS_EQ_TOL`
     (0.1%) count as equal; above the push ratio the push handles it.
3. **Did not move:** stay awake only if something is still possible: void beside it, a pressure
   difference or a merge with the same gas, a push, a balance, a mixing move, or buoyancy
   (including bulk buoyancy and spreading). A room of gas at one pressure (including mixed air,
   or any settled mixture) sleeps.

Pressure spreads by diffusion, sped up by the long jumps: a 2:1 pressure step across a 128-cell
room (6 m) evens out to 1% in about 5 s. Gas released into vacuum spreads at a few metres per
second, not at the speed of sound.

**Phase changes with mass.** Boiling water keeps its 125 g: one steam cell at very high pressure,
which then pushes and splits until it fills about 1700 cells at 1 atm. Steam condensing becomes a
full water cell (125 g) with probability `m / m_water`, otherwise nothing (deterministic hash of
seed, tick and cell), so mass is conserved on average. The condensation point does not depend on
pressure (a deviation: real steam at 1% of an atmosphere condenses at 7 °C, not 100 °C).

### Gas mixtures and bulk buoyancy

Code: `buoyancy_p()`, `gas_lift()`, `gas_spread_p()` and `gas_is_bubble()` in `src/sim/sim.c`.

Each cell holds one gas, so a mixture is a speckle of cells of each gas: a cell of CO2 among air
cells stands for CO2 mixed into the air molecule by molecule. Such a cell must not sink on its
own. If it did, every mixture would unmix (CO2 breathed out would rain onto the floor, a 50/50
mixture would separate into layers), when in reality a mixed gas stays mixed: its scale height
of separation is kilometres. What sinks is a **mixture that is denser in bulk**: a layer, a blob
or a plume, which moves as a whole and mixes at its edges. The gases compare windows of cells,
each gas at its density at 0 °C (`g_gas_buoy_density`; other cells not counted):

- **The test.** Two windows differ when the difference of their mean densities is at least
  `BULK_Z` (2) standard errors and at least `BULK_MIN_CONTRAST` (1%) as a contrast
  `(ρ_a − ρ_b) / (ρ_a + ρ_b)`. The standard error is the one the two windows would have if they
  held the same mixture: the pooled spread of their cells' densities. So the noise of a mixture
  never moves it, whatever the gases (a cell-level mix of H2 and CO2 is far noisier than one of
  CO2 and oxygen), while two pure gases differ at any contrast: a sharp layer or blob always
  moves.
- **Rising, lighter gas under a denser one.** The window above the cell (7 wide) is compared
  with the window below it (7 wide, from the cell down), and with the lighter of the two windows
  beside it (7 wide on each side, at the rows above; `SIDE_NOISE` = 1.5 times the standard
  errors, as the lighter of two is noisier). Windows are 4, 8 and 16 rows tall, and each stops at
  a cell of the column that is not gas: a sharp edge shows in the small ones, a smeared gradient
  only in the tall ones, which have less noise. The probability is
  `K_BUOY · min(1, BULK_K · contrast)`, with `BULK_K` = 4: a ball of CO2 in air (contrast 21%)
  sinks at up to a cell a tick, a few m/s, close to the speed `√(g' · d)` of a heavy blob a
  metre or so across.
- **Lift.** The parcel moves down one cell as a whole: the light cell is lifted to the top of
  the parcel (up the column while the cells are denser gases, or their 3-cell row has at least
  the density half-way between parcel and surroundings; at most `PARCEL_MAX` = 31 rows) and the
  column shifts down by one. So a blob sinks intact instead of crumbling cell by cell, and a
  heavy layer on top of a light one turns over, mixing as the light gas is lifted into it.
- **Bubbles.** A light cell alone in heavy gas rises (lifted again). Alone means the 3 cells
  above it are denser gases, and so are all but at most 2 of the 21 cells above it. Such a cell
  is left inside a parcel taller than `PARCEL_MAX`. In a mixture it always has its own kind
  near.
- **Spreading, a gravity current.** A light cell resting on something (not void, nor a gas
  lighter than the heavy one) trades places sideways with a denser gas next to it, where the gas
  in the 6 × 10 cells from the heavy cell on (at and above this row) is denser than on this
  side by `SPREAD_Z` (2.5) standard errors. So a pile of CO2 on the floor flattens out and fills
  pits, and a falling blob does not shed its sides (there is light gas under the air beside it).
  The test is stricter than for rising because moving sideways does not lower the gas, so
  nothing would settle a mixture it acted on.
- **Schedule (cost).** The window sums cost up to 450 cell reads, so a cell tries the 4-row
  windows one tick in `BULK_EVERY` (2) and spreading one in `SPREAD_EVERY` (8), each at that
  many times the probability. Every `BULK_FULL_EVERY` (16) ticks (all cells of a chunk on the
  same tick, chunks taking turns) it tries everything. Only then can a cell with a denser gas
  above or beside it fall asleep, so a settled mixture sleeps.
- A cell **warmer** than the different gas above it rises by itself (a thermal, as above):
  breath (at 37 °C, 4% CO2) rises off the face.
- Moves between gases of different densities wake every cell whose windows see them (11 cells
  to each side, 16 above and below). Oxygen and nitrogen share one density, so stirring air
  wakes nothing extra.

Tested: a 50/50 random mixture stays mixed and sleeps; air with 5% CO2 stays mixed; a ball of
CO2 high in a room of air sinks and pools over the floor; a CO2 layer on oxygen, and oxygen on
hydrogen, turn over; breathed-out CO2 stays mixed in a sealed room, so it reaches the danger
level (5 kPa) before oxygen falls to 10 kPa, as in real sealed rooms.

**Known limitations.**

- Different gases never share a cell. Total pressure evens out across them only cell by cell
  (2b's push, or the balance step), so two gases side by side can stay up to ~3% apart.
- Bulk buoyancy only sees differences above the noise of a mixture, so a thin CO2-rich haze over
  clean air sinks only where it is concentrated enough to stand out of its own noise (in reality
  it would sink, slowly). A heavy layer on top of a light one turns over, but it can stall with a
  weak top-heavy gradient left. A sinking blob leaves a faint trail of mixture, and lands as a
  low mound rather than a perfectly flat layer.
- A gas cell suddenly heated far above its neighbours expands partly by trading mass with them,
  which mixes some of its heat into them. Gradual heating (a hot plate) is barely affected, since
  the mass moved is proportional to the temperature step.

All constants live at the top of the gas section of `src/sim/sim.c` and in `src/sim/elements.h`.
Their current values are in `ELEMENTS.md`.

### Gas currents: moving matter stirs the air (M5)

Code: `gas_follow_wind()` and `stir_by_fall()` in `src/sim/sim.c`; sources in
`src/sim/creature.c`. A gas cell's `aux` holds a **current**: a direction (8, in 45° steps) and a
speed 0–31. The gas otherwise has no velocity: without currents, moving bodies and falling matter
would only swap the cells they touch, and a room's air would stay perfectly still.

- A gas cell with a current moves one cell along it with probability `speed / 32` per tick
  (swapping with the gas or void there), and the cell it pushes picks up the current minus
  `WIND_PASS_LOSS`. Blocked, the current turns 45° and loses `WIND_TURN_LOSS`. It spreads to a
  random neighbour (minus `WIND_SPREAD_LOSS`) half the time, and loses a step a quarter of the
  time: a current at full speed dies out in about 2 s. Cells with a current stay awake; then the
  air sleeps again (tested).
- **Sources:** a creature's step pushes the air in front of it (speed 12) and drags a wake behind
  it (18); each breath out blows a puff from the mouth (14, rising a little). Liquid or powder
  falling through gas kicks the displaced gas sideways and upward and drags the gas beside it
  down (6 plus the fall speed / 4).
- Currents move cells, so they **mix** the gases they pass through, and mass is conserved
  exactly (tested: walking along a striped room of oxygen and nitrogen, or sand falling through
  it, mixes the stripes there far more than in the same room left alone).

### Reactions (M5)

Code: `react()` in `src/sim/sim.c`, table `g_reactions` in `src/sim/elements.c`. A reaction turns a
fuel cell `a` and an oxidiser `b` into a product, using `b_per_a` kg of b per kg of a and
releasing `heat_per_kg` J per kg of a. So far one: **hydrogen burning**, 2 H₂ + O₂ → 2 H₂O
(steam): 7.94 kg of O₂ per kg of H₂, 120 MJ per kg of H₂ (lower heating value), igniting at
773 K.

- Checked first in a fuel cell's update, only in chunks where something is hot enough to ignite
  (the chunk or one around it has a temperature at or above the lowest ignition temperature), and
  only if the cell or one of its 4 neighbours is at least `ignite_k`.
- The partner is the nearest oxidiser cell within `REACT_REACH` (3 cells, square rings from a
  random corner). Burning gases mix molecule by molecule; with one gas per cell, the push of the
  hot products would otherwise leave fuel and oxidiser in separate pockets.
- **Mass is exact:** with enough oxidiser, the fuel cell becomes the product holding the fuel and
  the oxidiser used (the oxidiser cell keeps the rest); with too little, the oxidiser cell becomes
  the product and the fuel keeps what is left.
- **Heat:** the product's temperature is the reactants' heat plus the heat released, over its heat
  capacity, but at most `FLAME_MAX_K` (3000 K): hotter, real flames lose energy to dissociation,
  which is not modelled (the excess is dropped; a deviation).
- The hot, dense product (10 atm and more) pushes the gas around and ignites its neighbours: a
  flame front that crosses a room of stoichiometric hydrogen and oxygen in under a second
  (tested: most of the hydrogen burns; mass is conserved; the steam is hot and does not condense
  yet). Cold mixtures never burn (tested). Light a mixture with the Heat brush.

### Free particles (M4b; D-026)

Code: `src/sim/particle.c`. Fast-moving matter leaves the grid while it flies, as in Noita, and
lands back into it.

- A particle holds a position and velocity in floating point (cells, cells per tick), the `Cell`
  it carries and its stored temperature. While it flies, it is not on the grid (the cell it left
  is empty) and exchanges no heat. Only liquids fly so far.
- **Launched by** jets (Liquid pressure → Jets) and splashes (Liquids → fall speed).
- **Each tick, after the movement pass**, serially in list order: gravity
  (`PARTICLE_GRAVITY` = 9.81 m/s² = 0.0545 cells/tick²), speed limited to
  `PARTICLE_MAX_SPEED` (24 cells/tick), then the move in half-cell sub-steps. It flies through
  void, gas and falling liquid (a jet crosses a thin falling stream), and not diagonally between
  two blocked cells. When the next cell blocks it, it **lands** in the cell it is in.
- **Landing** puts its cell back on the grid. Gas there is squeezed into the gas of its kind
  around (spread out, `world_gas_region`). If something has settled into that cell, it lands in
  the nearest free cell (void or gas): straight up first, then rings up to `LAND_SEARCH` (8)
  away, then as a last resort up through liquid to its surface. If it still cannot land, it
  waits there and tries again next tick. Landing while moving down with nothing below (it hit
  something sideways), it keeps falling as a grid cell with its fall speed.
- The list holds `PARTICLE_MAX` (131072). When it is full, jets fall back to placing cells, and
  splashes are skipped (they are allowed only while it is under half full).
- **Falling streams stay on the grid** (D-014 still applies): turning every falling cell into a
  particle would cost far more for little gain. Particles carry jets and splashes.

**Known limitations.** A jet through a hole a few cells wide comes out in pulses rather than a
steady stream (the cells at the opening do not all ask every tick). Water painted in mid-air is
still for a tick before it starts falling, and a round blob can briefly jet sideways from its own
sides. A particle crossing a falling stream does not mix with it.

### Creatures (M4c; D-027)

Code: `src/sim/creature.c`. Creatures are entities, not cells: each has a position, a velocity and
a body, a 9 × 35 rectangle (45 cm × 1.75 m) of `ELEM_BODY` cells stamped into the grid. Body
cells are `BEHAVIOR_STATIC`, so liquid, gas, particles and water pressure treat them as walls, and
brushes do not paint over them; heat conducts through them. Creatures are updated serially, in
list order, at the start of each tick (before heat and movement).

- **Moving.** Gravity (9.81 m/s²) and, in liquid, buoyancy for the submerged fraction of its sides
  (body 985 kg/m³ against water's 1000: it floats, barely), drag, and swimming up while its head
  is under. Walking at 1.4 m/s (half in liquid or when impaired), stepping up ledges of up to
  `STEP_UP` (4 cells, 20 cm). It moves cell by cell; each step swaps the cells its leading edge
  moves into (void, gas or liquid) with the body cells it leaves at the other end, so gas and
  water flow around the body and **nothing is created or lost** (tested). Walking through a gas
  boundary carries the gas it walks into to behind it.
- **Breathing,** every `BREATH_TICKS` (4.3 s, 14 a minute). It takes the oxygen it uses
  (250 mL/min at rest, ×3 walking) from the oxygen within 16 cells of its mouth (at most half of
  each cell), and breathes out CO2 for what it used (200 mL/min): into CO2 near the mouth, or as
  a new CO2 cell at the mouth (the gas there squeezed into its kind around). Each breath also
  **stirs** the air: `STIR_CELLS` (16 cells, 2 L) times the speed-up and work, swapping gas
  within 16 cells of the mouth with gas up to 64 cells away. Without it, the air next to the face
  goes stale while the room stays fresh (gases mix only at interfaces; D-005), and a person
  suffocates long before the room's oxygen runs low. Underwater it gets no oxygen.
- **Health** from the partial pressures around the head (window of 16 cells around the mouth,
  smoothed over breaths): O2 below 16 kPa → impaired (slower); O2 below 10 kPa, CO2 above 5 kPa,
  or a breath that found less than half its oxygen → in danger, losing health (dead after 120 s);
  otherwise recovering (600 s to full). Tested: in a sealed 6.3 × 3.1 m room (one cell deep:
  1 m³ of air), one person at rest reaches danger after about 3.5 hours of game time, from CO2
  (5 kPa), within 25% of the 4 hours its CO2 output predicts for evenly mixed air (oxygen would
  last 7.2 hours). Breathed-out CO2 stays mixed in the air (D-030); it is a little more
  concentrated near the face.
- **Body heat:** held at 37 °C, giving up to 100 W (×3 walking), scaled by the gas conduction
  speed-up like the heat the body loses (`HEAT_TIME_SCALE_GAS`). Tested: in cold air the body
  stays at 37 °C and warms the air around it.
- `metabolism_scale` (1 = real time) speeds up breathing, stirring and health together, for
  gameplay or tests; the room-size results do not depend on it.
- The test creature (key K) wanders: it picks a direction about every 3 s and turns back when
  blocked.
- **The player** (M6a, `scene_spawn_player`) is a creature with a 9 × 37 body (1.85 m), running
  at 4 m/s (`speed`). It jumps when asked (`jump`) while standing: take-off at 3.13 m/s, about
  50 cm high (tested: 10 cells, and back on the floor). It faces the mouse (`aimed`), not its
  walking direction. In the air it keeps its momentum (no air control). Input is applied between
  ticks on the main thread.

### Rifle and bullets (M6a; D-031)

Code: `src/sim/weapon.c` (firing), `bullet_step()` in `src/sim/particle.c` (flight).

- **Firing.** While the trigger is held, the rifle fires every `RIFLE_INTERVAL` (5) ticks (720
  rounds/min) from the shooter's shoulder (row 8 of the body, 1.45 m up) toward the aim point,
  with a random spread of ±0.3°. The bullet is a free particle carrying one cell of lead
  (`ELEM_BULLET`, 400 K), at 900 m/s (300 cells/tick). It starts at the last body cell on the line
  of fire, so it never hits the shooter. The spent casing (`ELEM_CASING`, 370 K) is thrown up
  and back (1.5–4.5 m/s up) from the ejection port, 7 cells along the rifle, or from above the
  head if the port is inside a wall.
- **Flight.** A bullet faster than `BULLET_FAST` (6 m/s) covers its whole path for the tick in
  half-cell steps, at the end of the tick with the other particles (serial, deterministic, not
  bound by the 32-cell reach). Through void, gas and falling liquid it flies on, and gives each
  gas cell it crosses a current along its path (`BULLET_WIND`, 16): it stirs the air.
- **Impacts.** Each cell of dirt or powder it enters costs it 50 cells/tick of its 300 (about
  30 cm of earth or sand stops it). The cell is knocked out of the grid as debris: a particle
  (dirt loosened to soil) thrown back out of the hole at 4–12 m/s, which lands as a pixel. Rock:
  it chips one cell (rubble, thrown out) and stops. Liquid costs 15 × ρ/1000 per cell (about 1 m
  of water), and the first liquid cell it enters splashes out. Iron, ice, bodies and the world's
  edge stop it in front of them. After each dug cell, small clumps around it break loose.
- **Falling.** Stopped, the bullet drops from the last cell it reached (bouncing back at 0.3
  cells/tick) as an ordinary particle and lands as a powder pixel. Repeated shooting leaves piles
  of bullets and casings.
- Nothing is destroyed: dug cells become debris and land again (if the particle list is full,
  the cell is only loosened in place). Tested: ten shots into dirt dig at least 20 cells, dirt +
  soil is unchanged, 10 bullets and 10 casings lie on the ground (the casings near the shooter),
  nothing is left in flight; bullets stop at iron and drop in front of it; the air along a
  bullet's path has a current; the result is the same on 1 and 4 threads.

**Known limitations.** The body is a rectangle and does not bend, crouch or climb. It is a cell
thick, like everything (the world is a 5 cm slice). Wading only slows it down. Falling does no
damage. Exhaled air is not humid.

## Heat pass (M3)

Code: `src/sim/heat.c` (conduction, phase changes), `src/sim/sim.c` (convection), tables in
`src/sim/elements.c`. Every non-void cell has a temperature; painted cells start at their
element's spawn temperature (20 °C; ice −10 °C, steam 110 °C).

### Stored and physical temperature (latent heat)

The `temp` array holds a cell's **stored temperature**, which doubles as its heat content
(enthalpy): beyond a phase-change point, the excess is latent heat on its way to changing phase.
Water stored at 150 °C is water *at 100 °C* that has absorbed `c·50 K` toward boiling. The
**physical temperature** `elem_temp(e, T)` stops at the transition point; it is what conduction,
buoyancy, glow and the inspector use. The inspector shows the progress ("boiling 9%").

### Conduction

Each cell has a thermal mass `C = ρ · V_cell · c` (J/K). Between 4-neighbours i and j:

```
k_eff  = 2·k_i·k_j / (k_i + k_j)                 // two half-cells in series
a_ij   = min(k_eff · dx · dt · speed-up,          // J/K per tick
             (C_i·C_j / (C_i + C_j)) / 4)         // stability limit
T_i'   = T_i + Σ_j a_ij · (T_j − T_i) / C_i       // physical temperatures on the right
```

- `a_ij` is a precomputed symmetric table (`g_heat_coef`), so what one cell gains its neighbour
  loses: **energy is conserved exactly** (tested), and stays so whatever the thread count.
- **Gases:** a gas cell's thermal mass is its actual mass times c (`m · c`), so thin gas holds
  little heat. Any pair involving a gas uses `min(raw coefficient, C_min / 8)` computed per pair
  (`g_heat_coef_raw`); `C_min / 8` never exceeds the table's limit and keeps the step monotone.
- The limit makes every step monotone: one tick never pushes two cells past their common
  temperature, even with all four neighbours pushing the same way. There is no overshoot with
  extreme contrasts, e.g. 3000 K hydrogen on 100 K iron (tested).
- **Void has no heat capacity and exchanges nothing.** Vacuum insulates perfectly (D-017).
- **Jacobi:** all new temperatures are computed from the old ones into `temp_next`, then written
  back. Both passes run in parallel, one job per chunk.

**Which chunks run.** A chunk is heat-active while some temperature in it changed by more than
`HEAT_SLEEP_K` (0.001 K) in the last tick, or when an edit or a phase change touched it. Each tick
computes the active chunks **and their neighbours**, and cells only exchange with cells in chunks
computed the same tick, so no energy leaks at the edge of the active region. Moving cells carry
their temperature. Each chunk keeps the range of temperatures in it, and cells moving in only
reactivate heat if temperatures actually differ in or around the chunk. So a world at one
temperature costs nothing, however much moves.

### Speed-up

Real conduction is slow at 5 cm cells: heat crosses one cell of iron in about 2 minutes, rock in
half an hour, still water in hours. Conduction is sped up by `HEAT_TIME_SCALE` = **1000** for
solids and liquids. With it, a 1 m iron bar heats through in about a minute and a half, rock is
about 17× slower, and water conducts slowly (convection does the real work). Relative behaviour
stays realistic.

Gases get only **10×** (`HEAT_TIME_SCALE_GAS`), and so does any contact involving a gas. In air,
convection (which runs in real time) must beat conduction. With 1000× conduction, a pocket of hot
air lost its heat to its surroundings before it could rise (tested: hot gas rises).

### Convection

- **Moving cells carry their temperature**, including water-pressure transfers (tested with a
  U-tube whose hot side flows into the cold side).
- **Gases:** density follows the ideal gas law, `ρ(T) = ρ_ref · T_ref / T` (`T_ref` 0 °C, steam
  100 °C). Buoyancy compares densities at the cells' physical temperatures, including the same gas
  above when it is colder by more than `CONVECTION_MIN_K`. Warm air rises through cold air. When
  temperatures are equal, the precomputed integer table is used (fast path).
- **Liquids:** a still cell with the same still liquid above it that is denser (colder, for
  water) swaps with it with probability `min(1, K_CONVECTION · Δρ/ρ)`; `K_CONVECTION` = 200, so
  water 10 K warmer at 25 °C rises with p ≈ 0.5. Water uses its real density curve (Thiesen;
  densest at 4 °C), so ponds cool to 4 °C at the bottom and freeze from the top. Other liquids get
  a generic expansion coefficient. Such swaps do not relabel water-pressure bodies (same body).
- Where temperatures change by more than `HEAT_WAKE_MOVEMENT_K` (0.02 K/tick), the chunk's fluids
  are simulated so convection can start. Cells that could convect stay awake until it is done.
- Tested: a pool 100 cells deep, warm only at the bottom, warms at the top within 20 s.

**Known limitation:** buoyant gas rises cell by cell, exchanging places with the cold gas above
it, so a hot pocket rises while mixing rather than as a tight plume.

### Phase changes

Ice ↔ water at 0 °C (L = 334 kJ/kg), water ↔ steam at 100 °C (L = 2257 kJ/kg), rock ↔ lava at
1500 K (L = 400 kJ/kg) and iron ↔ molten iron at 1811 K (L = 247 kJ/kg; M5), all at 1 atm
(the boiling point does not depend on pressure yet). After each conduction step, a cell
whose stored temperature is far enough past its transition point that the heat beyond it pays for
the latent heat, `c · (T − T_thr) ≥ L`, changes phase. It keeps the remainder:
`T_new = T_thr ± m·(c·|T − T_thr| − L) / C_new`, where m is the cell's mass (a gas cell's own;
see Gases for boiling and condensing). The full latent heat acts as a natural
hysteresis (no flicker). A melted, frozen, condensed or boiled cell wakes its neighbourhood
(movement, rendering, water pressure).

## Randomness

- RNG: xorshift32 (`src/core/rng.h`), one per chunk job, reseeded at the start of every chunk
  update from `splitmix64(world.seed ^ splitmix64(tick) ^ (cx << 32 | cy))`.
- Player edits use their own generator, seeded from the world seed, the tick and the stroke
  position, so an edit stream replays identically.
- Use it for: random direction choices, swap probabilities, and the `shade` of new cells.
- Never use `rand()`. Never share an RNG between threads. Never let the outcome depend on thread
  count (see `ARCHITECTURE.md` → Determinism).

## Conservation checks (tests)

- Movement never creates or destroys liquid or solid cells: their per-element counts stay
  constant without edits or phase changes.
- Gas mass is conserved per gas (gas cells split and merge, so their count changes).
- Without boundary losses, total thermal energy `Σ C_i·T_i` stays constant within float tolerance
  (M3); a gas cell's C is its mass times its specific heat.
