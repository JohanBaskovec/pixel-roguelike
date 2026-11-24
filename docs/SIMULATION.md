# Simulation

How matter behaves. Code lives in `src/sim/`. Every rule here needs a behaviour test in `tests/`.

## Units and scale

- **SI units internally.** Temperatures are in **Kelvin** (shown as °C in the UI), density in
  kg/m³, heat capacity in J/(kg·K), conductivity in W/(m·K).
- **Physical cell size: 1 px = 5 cm** (decided, D-037), and each cell is treated as a cube
  (`CELL_SIZE_M = 0.05`, volume 1.25 × 10⁻⁴ m³). A water cell therefore weighs 0.125 kg. The
  world is a slice one cell (5 cm) deep.
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
| `GAS` | flows by pressure, diffuses, rises or sinks by density | gas: a mixture of oxygen, CO2, hydrogen, water vapour and nitrogen (see Gases) | M1 (mixtures M6a) |
| `POWDER` | falls, slides diagonally, piles up; no sideways spread; sinks slowly in liquids | sand, loose soil, rubble | M5 |

**Void** is vacuum: no mass, no heat capacity, no conduction. It is what a cell becomes when
matter leaves it.

## Movement pass

### Scan order, phases and the tick stamp

- Chunks are processed in the 4-phase checkerboard (`ARCHITECTURE.md`). The phase that goes
  first rotates every tick, so no chunk group always gets first pick of free space at its borders.
- Within each chunk's dirty rect (only its woken 16 × 16 tiles, `ARCHITECTURE.md` → Dirty rects),
  rows are processed **bottom to top**. Within a row, the
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

Density comparisons use a pre-quantised integer table (`g_elem_density_q`, g/m³); a gas cell
counts as air there, so a liquid or powder always displaces gas. Gas against gas compares the two
mixtures' real densities (see Gases).

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
  sealed at the top (a barometer; tested at half an atmosphere, 103 cells, to keep the test
  world small); water flowing into a sealed room of air compresses the air
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
the gas cells around Q, and the gas above the source **expands** into the cell the water
left. Both are spread over up to `GAS_REGION_CELLS` (256) gas cells within
`GAS_REGION_RADIUS` (16), found by a breadth-first walk through gas (`world_gas_region`): one
cell of volume moved at a single spot would change its pressure by tens of percent, which is
207 × that many cells of water head, and the water would chase those spots in a cascade. Gas
equalises by diffusion, much slower than in reality, so such compressing transfers are limited to
one per body every `GAS_LIQUID_INTERVAL` (2) ticks: the rest of the gas keeps up, and a barometer
fills in about 20 s. Gas next to a liquid surface that changes (a transfer, heating) triggers a
relabel, so heating a sealed tank pushes its water too. If the gas at Q has no gas around it to be
compressed into (a lone bubble under the surface), the transfer does not happen: trading places
would carry the bubble to the source, which may be far away, even into the vacuum of a sealed
barometer tube. The bubble rises on its own.

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
- Gas pressure is resolved to one cell of liquid (490 Pa for water). Gas pressure itself evens
  out only to within 0.3% between neighbouring cells (about 300 Pa at 1 atm, see Gases), which
  is under one cell of water.
- A gas bubble inside a liquid is just a lighter fluid that rises; its pressure does not follow
  its depth.
- Transfers move a cell from the top of the body to the opening in one step. Visually this reads
  as the surface lowering and the water flowing out, but water does not travel through the body.

### Gases (M4; mixtures since M6a, D-035)

Code: `update_gas()` and the functions before it in `src/sim/sim.c`; `GasMix` and its helpers in
`src/sim/world.h`.

**A gas cell is a mixture.** Every gas cell is the element `ELEM_GAS` and holds a mass of each gas
species (`World.gas`, a `GasMix` of kg: O₂, CO₂, H₂, water vapour, N₂) at one temperature. The
species elements (`ELEM_OXYGEN`...) name the species and give their properties (molar mass, heat
capacity, conductivity, colour); no cell holds them. Painting oxygen paints a gas cell of oxygen
alone; the **Air** brush paints air, the same mixture in every cell: 20.95% O₂, 0.04% CO₂ and the
rest nitrogen (78.1% N₂ and 0.9% argon counted as N₂) by moles, at 1 atm, dry. Pressure follows
the ideal gas law for the cell's moles, `P = n · R · T / V_cell`; partial pressures are each
species' share. Mass is conserved by every rule below, species by species (tested to 1e-4), except
condensation (on average).

Each tick, a gas cell:

0. **Sleeps at once** if it is still (no current, nothing hot near, no water vapour) and exactly
   like every gas neighbour (same masses and temperature), with no void or liquid beside it:
   still air costs a few loads. A cell like its neighbours only **within tolerance** (pressures
   within `GAS_EQ_TOL`, 0.3%; mole fractions within `GAS_MIX_TOL`, 0.1%; densities within
   `GAS_BUOY_TOL`, 0.02%; and the pressure of the cells `GAS_REACH` (16) to the right and below
   within `GAS_EQ_TOL_FAR`, 0.2%, so a smooth pressure gradient does not freeze) also sleeps;
   this second check runs after steps 1 and 2.
1. **Follows its current** (gas wind, see Gas currents): moving air carries the gas along.
2. **Burns** (Reactions) and **condenses** water vapour (Phase changes) when that applies.
3. **Buoyancy.** It rises through denser gas above. Densities are compared **at 1 atm** for each
   cell's temperature and molar mass (`World.rho`, recomputed each tick for the awake chunks),
   whatever its actual pressure: a parcel at another pressure would be squeezed or expanded to
   its surroundings' pressure long before it rose or sank. So warm air rises, CO₂-rich air sinks,
   hydrogen rises, and **an even mixture is the same everywhere, so it does not unmix** (tested).
   Not only against the cell above: diffusion blurs every boundary, so a heavy layer on a light
   one differs little from cell to cell, yet the columns' weights differ. Mean densities over
   `GAS_BUOY_SCALES` windows of the cell's column (1, 4, 8 and 16 rows above it, against as many
   from it down) give a contrast at each height; the drive is the largest contrast times
   √height (bigger structures rise faster: buoyant speed grows as the square root of their size),
   and the cell rises with probability `K_BUOY · drive`. At height 1 it trades places with the cell
   above; at a bigger one it is **lifted** to the top of the denser gas above it (up to that height)
   and that column shifts down a row, so a heavy layer or blob sinks as a whole. (A quick test
   first: gas clearly lighter just above means stable.) As a **bubble** under liquid it rises
   through it.
4. **Heavy gas spreads sideways** (a gravity current). A cell resting on something (not on lighter
   gas), next to lighter gas, compares the weight of the `GAS_SPREAD_H` (8) rows at and above it
   with that of the column 1, 4, 8 or 16 cells to the side (the hydrostatic pressure at its height;
   a gravity current is driven by the difference over its length): drive = the largest contrast ×
   √distance. Moving, the row from it to the shortest distance showing most of the contrast shifts
   one cell outward, and the gas there takes its place (the light gas flowing back over the
   current): beside a sharp boundary that is the next cell, at a smooth pool edge further. A pool
   of CO₂ flattens over the floor; cold air flows under warm air.
5. **One random direction** (4-neighbourhood):
   - **Void:** a dense cell (at least `GAS_SPLIT_MOLES`, 2% of the moles of 1 atm) **splits**: half
     its gas goes 1..spread cells into the void (spread: 3 for air, more for lighter gas, as √ of
     the molar mass ratio, 1..6). A thinner cell moves whole, as a particle.
   - **Gas, both thin:** the two **merge**.
   - **Gas: diffusion.** Each species moves down its mole-fraction difference, `GAS_DIFFUSE_K`
     (3.3%) of it times the cells' mean moles per tick, which moves no net moles (pressure is
     untouched) and carries each species' heat. Each pair is picked about every other tick:
     effective diffusivity 2.5e-3 m²/s, gentle indoor turbulence, about 150× molecular (D-032).
     A current multiplies it by `1 + GAS_TURB_PER_WIND · speed` (eddy diffusivity, as for heat),
     up to evening out the pair.
   - **Gas: pressure equalisation** (only where a pressure difference was found). Half the time
     the full `GAS_REACH` (16), else a random 1..16 cells, along that direction through gas, to the
     last gas cell (the adjacent one if their temperatures differ by more than
     `GAS_JUMP_MAX_DT`, 2 K: long jumps between different temperatures would smear heat). The
     sender gives half the difference of moles × temperature, with its composition and heat
     (temperatures mix by heat capacity; energy is conserved). Pressures count as equal within
     `GAS_EQ_TOL` (0.3%) for partners under 8 cells apart and `GAS_EQ_TOL_FAR` (0.2%) further, so a
     smooth gradient evens out to about 0.2% per 16 cells of path (tested: two rooms at 2 and 1 atm
     joined by a gap end at 1.5 atm, within 3%); differences under 0.3% of 0.1 atm always count as
     equal. A 1700-atm cell of boiling water vapour gives half its gas each time: it expands within
     ticks. Thin gas drifting in a vacuum (under `GAS_SPLIT_MOLES`, touching void) is not fed by gas
     more than `GAS_FEED_RATIO` (2.1) times denser: dense gas expands into a vacuum by splitting,
     otherwise every drifting cell would grow dense and split again.
6. Otherwise, if something is still possible (step 0), it stays awake.

Pressure spreads by diffusion, sped up by the long jumps: a 2:1 step across a 128-cell room (6 m)
evens out within a few seconds. Gas released into vacuum spreads at a few metres per second, not at
the speed of sound.

**Tested:** a blob of oxygen fills a vacuum box ten times its size at 0.1 atm; sealed gas follows
P ∝ T; painted air and even mixtures (50% and 5% CO₂) stay put and sleep; nitrogen over oxygen
diffuses as √t at the game's diffusivity; CO₂ over oxygen and oxygen over hydrogen turn over within
seconds; a ball of CO₂ sinks, spreads over the whole floor within 10 s and stays a CO₂-rich layer
under clean air, thinning; a warm pocket rises; the same result on 1 and 4 threads.

**Known limitations.** Buoyant motion is a cell or a column shift at a time, a coarse stand-in for
flow; a 50 cm pocket of air 30 K warmer than the room rises at about 0.2 m/s (measured), slower
than a real one. A gradient gentler than
the tolerances freezes (asleep) until something wakes it. Gas pressure is not hydrostatic (air's
own weight, 12 Pa per metre, is ignored).

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
  it (18); each breath out blows a puff from the mouth (14, rising a little); a bullet leaves a
  current along its path (8), and the muzzle blast a full one. A stream of liquid or powder
  falling through gas (falling matter just above or below the falling cell) kicks the displaced
  gas sideways and upward and drags the gas beside it down (6 plus the fall speed / 4); a lone
  falling cell (a clod, a drop, a spent bullet) only kicks the gas it displaces, at 4
  (`FALL_WIND_LONE`: it never spreads and dies in about 16 ticks).
- **Currents stop in uniform gas.** A gas cell with a current whose four neighbours hold exactly
  its mixture at exactly its temperature (or are solid) loses its current at once: moving it
  would trade identical contents, and the mixing and heat exchange it speeds up have nothing to
  act on. Currents act only where the gas is not uniform (smoke, breath, muzzle gas, warm air,
  next to liquid or vacuum). The cost: a gust does not carry across clean air into smoke beyond
  it. Without this, debris falling through a clear sky (bullets fired at a dirt ceiling) kept
  1.8 million cells stirring for nothing.
- Currents move cells, and they **mix** the gases they pass through: diffusion and heat exchange
  between gas cells are multiplied by `1 + GAS_TURB_PER_WIND · speed` (turbulence, D-034). Mass is
  conserved exactly (tested: walking along a striped room of nitrogen and a blend of the same
  density, or sand falling through it, mixes the stripes there far more than in the same room
  left alone).

### Reactions (M5)

Code: `react()` in `src/sim/sim.c`, table `g_reactions` in `src/sim/elements.c`. A reaction turns a
fuel cell `a` and an oxidiser `b` into a product, using `b_per_a` kg of b per kg of a and
releasing `heat_per_kg` J per kg of a. So far one: **hydrogen burning**, 2 H₂ + O₂ → 2 H₂O
(steam): 7.94 kg of O₂ per kg of H₂, 120 MJ per kg of H₂ (lower heating value), igniting at
773 K.

- **In the cell** (D-035): a gas cell holding the fuel and the oxidiser burns when it, or one of
  its 4 neighbours, is at least `ignite_k`; checked only in chunks where something is hot enough
  (the chunk or one around it). All the fuel the oxidiser allows reacts at once into the product,
  in the same cell: **mass is exact**.
- **Heat:** the heat released goes into the cell (its mixture's heat capacity), up to
  `FLAME_MAX_K` (3000 K): hotter, real flames lose energy to dissociation, which is not modelled
  (the excess is dropped; a deviation).
- The hot product expands and heats its neighbours, which ignite: a flame front that crosses a
  room of stoichiometric hydrogen and oxygen (premixed: knallgas) in a few seconds (tested: most
  of the hydrogen burns; mass is conserved; the vapour is hot and does not condense yet). Cold
  mixtures never burn (tested). Hydrogen meeting air burns where the two have mixed. Light a
  mixture with the Heat brush.

### Free particles (M4b; D-026)

Code: `src/sim/particle.c`. Fast-moving matter leaves the grid while it flies, as in Noita, and
lands back into it.

- A particle holds a position and velocity in floating point (cells, cells per tick), the `Cell`
  it carries and its stored temperature. While it flies, it is not on the grid (the cell it left
  is empty) and exchanges no heat. Liquids fly (jets, splashes), and since M6a so do bullets,
  casings, debris and kicked or lifted powder.
- **Launched by** jets (Liquid pressure → Jets), splashes (Liquids → fall speed), the rifle and
  bullet impacts (Rifle and bullets), and bodies kicking or lifting powder (Creatures).
- **Each tick, after the movement pass**, serially in list order: gravity
  (`PARTICLE_GRAVITY` = 9.81 m/s² = 0.0545 cells/tick²), speed limited to
  `PARTICLE_MAX_SPEED` (24 cells/tick), then the move in half-cell sub-steps. It flies through
  void, gas and falling liquid (a jet crosses a thin falling stream), and not diagonally between
  two blocked cells. When the next cell blocks it, it **lands** in the cell it is in.
- **Landing** puts its cell back on the grid. Gas there is squeezed into the gas cells around
  (spread out, `world_gas_region`). If something has settled into that cell, it lands in
  the nearest free cell (void or gas): straight up first, then rings up to `LAND_SEARCH` (8)
  away, then as a last resort up through liquid to its surface. If it still cannot land, it
  waits there and tries again next tick. Landing while moving down with nothing below (it hit
  something sideways), it keeps falling as a grid cell with its fall speed.
- The list holds `PARTICLE_MAX` (131072). When it is full, jets fall back to placing cells, and
  splashes are skipped (they are allowed only while it is under half full).
- **Falling streams stay on the grid** (D-014 still applies): turning every falling cell into a
  particle would cost far more for little gain. Particles carry what flies fast.
- A fast bullet is a particle with its own step (not bound by the 32-cell reach, as particles
  run serially): see Rifle and bullets. Powders in flight pass through creatures' bodies.

**Known limitations.** A jet through a hole a few cells wide comes out in pulses rather than a
steady stream (the cells at the opening do not all ask every tick). Water painted in mid-air is
still for a tick before it starts falling, and a round blob can briefly jet sideways from its own
sides. A particle crossing a falling stream does not mix with it.

### Creatures (M4c; D-027)

Code: `src/sim/creature.c`. Creatures are entities, not cells: each has a position, a velocity and
a body, a 5 × 17 rectangle (25 cm × 85 cm: half a person's size, D-038) of `ELEM_BODY` cells
stamped into the grid. Body
cells are `BEHAVIOR_STATIC`, so liquid, gas, particles and water pressure treat them as walls, and
brushes do not paint over them; heat conducts through them. Creatures are updated serially, in
list order, at the start of each tick (before heat and movement).

- **Moving.** Gravity (9.81 m/s²) and, in liquid, buoyancy for the submerged fraction of its sides
  (body 985 kg/m³ against water's 1000: it floats, barely), drag, and swimming up while its head
  is under. Walking at 1.4 m/s (half in liquid or when impaired), stepping up ledges of up to
  `STEP_UP` (2 cells, 10 cm). It moves cell by cell; each step swaps the cells its leading edge
  moves into (void, gas or liquid) with the body cells it leaves at the other end, so gas and
  water flow around the body and **nothing is created or lost** (tested). Walking through a gas
  boundary carries the gas it walks into to behind it.
- **Liquid flows around a body moving up or down** (`flow_around`). Where the fluid it moves into
  is not the kind of fluid beyond its trailing end (landing in a pool: water under the feet, air
  over the head; jumping out of one: air over the head, water under the feet, or the floor, then
  the water beside the feet), the cell does not pass through the body: it goes to the pool's
  surface beside the body (within `FLOW_SIDE`, 8 columns, wherever the surface is lowest when the
  body sinks in, highest when it rises out), and the surface cell fills the space the body leaves.
  Displaced water raises the surface around the body, and water fills the gap under the feet.
  (Before, landing in water or lava put it over the head, and a jump put air under the water.)
  Tested: falling into a pool, no water ever lands over the head; jumping in 85 cm of water, no
  air ever gets below the surface.
- **Powder in mid-air never blocks a body** (after play-testing: falling loose soil stopped the
  player). Powder falling through void or gas (its fall speed is set), and powder lying on a
  body, is "in the air": a body moving into it lifts it out of the grid as a particle, falling at
  its speed and sliding off sideways at 0.6 cells/tick (`LIFT_SIDE`), which passes through the
  body (powders in flight do) and lands on the ground beside it, then lies there like sand.
  Tested: walking into a falling column of soil does not stop the player, a jump with soil on the
  head goes its full height, and every cell lands again.
- **Breathing in and out,** every `BREATH_TICKS` (4.3 s, 14 a minute), the air as it is mixed
  around the mouth. **Breathing in** draws a tidal volume of 0.5 L (`TIDAL_CELLS`, 4 cells; ×3
  walking) into the lungs: the same fraction of every gas cell among the `DRAW_MIN` (32) gas cells
  nearest the mouth (more for a bigger breath: each gives at most half its gas), whatever the
  gas: nitrogen, oxygen, CO2, hydrogen. The lungs hold a mixture (`Creature.lungs`, a `GasMix`). The
  body then takes the oxygen it uses (250 mL/min at rest, ×3 walking) from the lungs, at most
  `O2_EXTRACT_MAX` (half) of what they hold, and adds the CO2 it makes for it (200 mL/min). At
  rest a breath goes in at about 21% oxygen and out at 17%, with 3% CO2 (real: 16% and 4%).
  **Breathing out**, after `INHALE_TICKS` (1.7 s), mixes the lungs' air back into the gas cells
  nearest the mouth (as many as it was drawn from) at 34 °C (`EXHALE_K`), or fills the mouth cell
  if it is void, with a puff away from the face (breathing in draws a gentler current toward it).
  So the breath out is warm and stale, and other gases breathed in (hydrogen, CO2) come back out.
  **With its mouth
  in liquid it holds its breath:** it does not breathe out, and the body keeps taking oxygen from
  the lungs (at most half of what is left each breath) until they run out. A dying creature
  breathes out its last breath. Water vapour is not breathed out yet.
  Tested: oxygen used and CO2 made match the real rates; O2, CO2 and nitrogen are conserved
  (counting the lungs); the first breath out leaves CO2 in front of the face; under water it
  holds its breath and puts no gas into the water. Each breath in also **stirs** the air: `STIR_CELLS` (16 cells, 2 L) times the speed-up and work, swapping gas
  within 16 cells of the mouth with gas up to 64 cells away. Without it, the air next to the face
  goes stale while the room stays fresh (the breath's own currents and diffusion are too slow for
  breathing sped up 200× in tests), and a person suffocates long before the room's oxygen runs
  low. Underwater it gets no new air.
- **Health** from the partial pressures around the head (window of 16 cells around the mouth,
  smoothed over breaths): O2 below 16 kPa → impaired (slower); O2 below 10 kPa, CO2 above 5 kPa,
  or a breath that found less than half its oxygen → in danger, losing health (dead after 120 s);
  otherwise recovering (600 s to full). Tested: in a sealed 6.3 × 3.1 m room (one cell deep:
  1 m³ of air), one person at rest reaches danger after about 4 hours of game time, from CO2
  (5 kPa), within 5% of what its CO2 output predicts for evenly mixed air (oxygen would last
  7 hours; the test runs breathing 200× faster, with stirring in proportion). Breathed-out CO2
  stays mixed in the air (D-035). In a smaller room (3.1 × 3.1 m) the warm breath collects near
  the head, and danger comes about 30% before the room's average CO2 reaches 5 kPa.
- **Body heat:** held at 37 °C, giving up to 100 W (×3 walking), scaled by the gas conduction
  speed-up like the heat the body loses (`HEAT_TIME_SCALE_GAS`). Tested: in cold air the body
  stays at 37 °C and warms the air around it.
- `metabolism_scale` (1 = real time) speeds up breathing, stirring and health together, for
  gameplay or tests; the room-size results do not depend on it.
- The test creature (key K) wanders: it picks a direction about every 3 s and turns back when
  blocked.
- **The player** (M6a, `scene_spawn_player`) is a creature with a 5 × 17 body (85 cm), running
  at 4 m/s (`speed`). It jumps when asked (`jump`) while standing: take-off at 3.13 m/s, about
  50 cm high (tested: 10 cells, and back on the floor). It faces the mouse (`aimed`), not its
  walking direction. **Air control** (a game deviation): in the air, the walking direction pulls
  its velocity toward walking speed by `AIR_CONTROL` (10% of it) per tick, so it reaches full
  speed in 0.17 s of a 0.64 s jump, and can jump straight up and then move (tested: 12+ cells in
  the air after a standing jump). Input is applied between ticks on the main thread.
- **Kicking small piles** (M6a, after play-testing: the player got stuck on its casings). When a
  walker's leading column holds powder (casings, spent bullets, loose soil, rubble, sand) only in
  its lowest `KICK_ROWS` rows (20 cm), and nothing else in the way, each of those cells is kicked
  ahead: a particle from the first free cell above it (in front, or in that column), at twice
  the walker's speed and 1.5–3.3 m/s upward, well clear of it. It lands again ahead (nothing is
  destroyed). It is not kicked if powder rises above the kick rows within `KICK_AHEAD` (6)
  columns: that is the foot of a bigger heap, and the walker climbs its 45° slope (`STEP_UP`)
  instead. (Kicking a heap's foot dug the walker into it until the face was too high to kick or
  climb, and pixels kicked onto a rising slope slid back against the walker: both left it stuck,
  found in play-testing.) Tested: walking through small piles of each (up to 20 cm), the player
  stays within 2 cells of the floor, and every cell is still there; heaps of loose soil 1 and 2 m
  high are crossed both ways.

### Rifle and bullets (M6a; D-031)

Code: `src/sim/weapon.c` (firing), `bullet_step()` in `src/sim/particle.c` (flight).

- **Firing.** While the trigger is held, the rifle fires every `RIFLE_INTERVAL` (5) ticks (720
  rounds/min) from the shooter's shoulder (row 4 of the body, 65 cm up) toward the aim point,
  with a random spread of ±0.3°. The bullet is a free particle carrying one cell of lead
  (`ELEM_BULLET`, at room temperature: see `ELEMENTS.md`), at 900 m/s (300 cells/tick). It starts at the muzzle, `RIFLE_LENGTH`
  (9 cells, 45 cm, at the body's half size) along the line of fire, or in front of a closer wall: well clear of the body even
  when the shooter moves forward at the start of the next tick, before the bullet flies (it used
  to start at the edge of the body, then stop in the body and drop in front of the player). The
  spent casing (`ELEM_CASING`, at room temperature) is thrown up and back (1.5–4.5 m/s up) from the ejection
  port, 7 cells along the rifle, or from above the head if the port is inside a wall.
- **Flight.** A bullet faster than `BULLET_FAST` (30 m/s) covers its whole path for the tick in
  half-cell steps, at the end of the tick with the other particles (serial, deterministic, not
  bound by the 32-cell reach). Through void, gas and falling liquid it flies on, and gives each
  gas cell it crosses a current along its path (`BULLET_WIND`, 8): it stirs the air.
- **Impacts.** Each cell of dirt or powder it enters costs it 50 cells/tick of its 300 (about
  30 cm of earth or sand stops it). The cell is knocked out of the grid as debris: a particle
  (dirt loosened to soil) thrown back out of the hole at 4–12 m/s, which lands as a pixel. Rock:
  it chips one cell (rubble, thrown out) and stops. Liquid costs 15 × ρ/1000 per cell (about 1 m
  of water), and the first liquid cell it enters splashes out. Iron, ice, bodies and the world's
  edge stop it in front of them. After each dug cell, small clumps around it break loose.
- **Muzzle gas** (`muzzle_gas()`): the propellant, 1.6 g (about a 5.56 mm cartridge's), leaves the
  muzzle behind the bullet as gas at 1500 K (`MUZZLE_K`, after expanding): 55% CO2, 27% water
  vapour, 18% nitrogen by mass. (Single-base powder gives about 55% CO2 and CO, 25% water vapour,
  18% nitrogen and 1–2% hydrogen. Carbon monoxide has no species and is counted as CO2; with the
  hydrogen it would burn in the air in a "secondary flash", which flash hiders suppress.) It is
  mixed, in equal shares, into the gas of the first `MUZZLE_REACH` (6) cells along the line of
  fire, which it heats and pressurises, and the gas there and either side of it gets a full
  current along the line of fire. The hot gas glows (the hot-matter glow) for a few ticks while its
  current mixes it, and its heat, into the air (turbulence), then expands and drifts as warm,
  humid, CO2-rich air; sustained fire in a closed room fills it with CO2. Tested: one shot puts
  0.88 g of CO2 and 0.43 g of vapour in front of the muzzle, above 1000 K; 20 ticks later no gas
  glows, and the air in front is still warm.
- **Falling.** Stopped, the bullet drops from the last cell it reached (bouncing back at 0.3
  cells/tick) as an ordinary particle and lands as a powder pixel. Repeated shooting leaves piles
  of bullets and casings.
- **Powders in flight pass through bodies** (`particle.c`, `flies()`): casings, spent bullets,
  debris and kicked cells fly through creatures' bodies and land only on the ground (or other
  matter), then lie there like sand. So a casing never lands on the shooter or against its body,
  where it would block it. (A fast bullet still stops at a body.)
- Nothing is destroyed: dug cells become debris and land again (if the particle list is full,
  the cell is only loosened in place). A spent bullet kicked or knocked out of a pile flies far
  slower than `BULLET_FAST`, so it falls as debris and does not dig. Tested: ten shots into dirt
  dig at least one full 30 cm (later bullets mostly hit the soil and spent bullets that fill the
  hole), dirt + soil is unchanged, 10 bullets
  and 10 casings lie on the ground (the casings near the shooter), nothing is left in flight;
  bullets stop at iron and drop in front of it; the air along a bullet's path has a current; the
  result is the same on 1 and 4 threads; running while firing ahead, then back over its own
  casings, the player covers more than 150 of the 160 cells it covers without shooting.

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
- **Gases:** a gas cell's thermal mass is its mixture's, `Σ m_s · c_s`, so thin gas holds little
  heat, and its conductivity the mole-fraction-weighted mean of its species' (an approximation).
  Any pair involving a gas computes its coefficient (two half-cells in series, the gas speed-up)
  and `min(that, C_min / 8)`; `C_min / 8` never exceeds the table's limit and keeps the step
  monotone. Each pair of neighbours is computed once per tick (a horizontal and a vertical pair
  per cell), and pairs at one temperature cost nothing.
- The limit makes every step monotone: one tick never pushes two cells past their common
  temperature, even with all four neighbours pushing the same way. There is no overshoot with
  extreme contrasts, e.g. 3000 K hydrogen on 100 K iron (tested).
- **Turbulence between gas cells.** Where either of two gas cells has a current (gas wind), their
  coefficient is multiplied by `1 + GAS_TURB_PER_WIND · speed` (23 per unit, up to 31): an eddy
  diffusivity of u·l (the current's speed, 2.9 m/s at full speed, times one cell) is about 700
  times air's thermal diffusivity as sped up. Most currents then exchange at the stability limit:
  stirred gas mixes its heat as fast as the grid allows, as turbulent air does (swapping cells
  moves gas, not heat between neighbours). Not between a gas and a solid or liquid (a surface
  keeps its boundary layer). Currents come from moving bodies, breath, bullets, falling matter and
  the muzzle; convection makes none. Symmetric, so energy is still conserved.
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
- **Gases:** a gas cell's density (at 1 atm) follows the ideal gas law for its mixture and its
  temperature, `ρ = M · P_atm / (R · T)`, so warm air is lighter than the same air cold and rises
  through it (see Gases → buoyancy; the same rule handles composition and temperature).
- **Liquids:** a still cell with the same still liquid above it that is denser (colder, for
  water) swaps with it with probability `min(1, K_CONVECTION · Δρ/ρ)`; `K_CONVECTION` = 200, so
  water 10 K warmer at 25 °C rises with p ≈ 0.5. Water uses its real density curve (Thiesen;
  densest at 4 °C), so ponds cool to 4 °C at the bottom and freeze from the top. Other liquids get
  a generic expansion coefficient. Such swaps do not relabel water-pressure bodies (same body).
- Where temperatures change by more than `HEAT_WAKE_MOVEMENT_K` (0.02 K/tick), the chunk's fluids
  are simulated so convection can start. Cells that could convect stay awake until it is done.
- Tested: a pool 100 cells deep, warm only at the bottom, warms at the top within 20 s.

**Known limitation:** buoyant gas rises a cell or a column shift at a time, so a hot pocket rises
slowly and smears rather than as a tight plume (measured: a 50 cm pocket 30 K warmer than the room
rises at about 0.2 m/s).

### Phase changes

Ice ↔ water at 0 °C (L = 334 kJ/kg), water → water vapour at 100 °C (L = 2257 kJ/kg), rock ↔ lava at
1500 K (L = 400 kJ/kg) and iron ↔ molten iron at 1811 K (L = 247 kJ/kg; M5); lead (spent bullets,
solid lead) ↔ molten lead at 600.6 K (327 °C, L = 23 kJ/kg) and brass (casings, solid brass) ↔
molten brass at 1208 K (935 °C, between cartridge brass's solidus and liquidus; L ≈ 168 kJ/kg;
M6a). Molten metal solidifies into a solid block (lead, brass), not back into bullets or casings.
Molten lead (10 660 kg/m³) sinks through everything else. Tested: bullets and casings dropped into
lava melt and sink to the bottom; cooled, they become lead and brass, every cell kept. All at 1 atm
(the boiling point does not depend on pressure yet). After each conduction step, a cell
whose stored temperature is far enough past its transition point that the heat beyond it pays for
the latent heat, `c · (T − T_thr) ≥ L`, changes phase. It keeps the remainder:
`T_new = T_thr ± m·(c·|T − T_thr| − L) / C_new`, where m is the cell's mass. The full latent heat
acts as a natural hysteresis (no flicker). A melted, frozen or boiled cell wakes its neighbourhood
(movement, rendering, water pressure).

**Boiling** water keeps its 125 g: the cell becomes a gas cell of water vapour alone at very high
pressure (about 1700 atm), which expands within ticks. **Water vapour condenses at its dew point**
(D-035; `condense()` in `src/sim/sim.c`): in a gas cell whose vapour is beyond saturation (Antoine's
equation, tabulated: 2.3 kPa at 20 °C, 1 atm at 100 °C), as much vapour condenses as warms the cell
back to the dew point (the latent heat goes into the gas) and no more than the excess, so it keeps
condensing as long as the gas is cooled. The condensed water becomes a droplet, a full water cell
(125 g) whose gas is pushed into the gas around, with probability mass / 125 g; otherwise it is
gone (deterministic per-chunk RNG), so mass is conserved **on average**. Saturation is taken at
1 atm total pressure (the vapour's mole fraction times 1 atm): a cell of vapour just boiled off,
at 1700 atm, does not condense back at once. So air can be humid: air with 1% vapour (dew point
7 °C) stays vapour at room temperature, and cooled below 7 °C it rains out (tested: humid air held
at 20 °C condenses to saturation, and the droplets hold the vapour lost). Water does not evaporate
below 100 °C (no evaporation from surfaces yet).

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
- Gas mass is conserved per species (gas cells split and merge, so their count changes), except
  where water vapour condenses (conserved on average) or hydrogen burns (the total is conserved).
- Without boundary losses, total thermal energy `Σ C_i·T_i` stays constant within float tolerance
  (M3); a gas cell's C is the sum of its species' masses times their specific heats.
