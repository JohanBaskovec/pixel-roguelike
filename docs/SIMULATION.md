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

### Scan order and the clock bit

- Within each chunk's dirty rect, rows are processed **bottom to top**. Within a row, the
  horizontal direction **alternates every tick**, so nothing drifts systematically to one side.
- `world.clock` flips every tick. When a cell is moved or updated, its flags' clock bit is set to
  the current clock. A cell whose clock bit already equals the current clock is skipped. This
  stops a rising gas, which moves into a row that has not been processed yet, from being updated
  twice in one tick.
- `STATIC` and `NONE` cells are skipped immediately.

### The "can enter" test (density swaps)

A moving cell **A** may move into target cell **B** if:

1. B is **void** → move (A goes to B's position; B's position becomes void), or
2. B is a **fluid** (liquid or gas), not static, and the densities favour it:
   - moving **down**: `density(A) > density(B)`
   - moving **up**: `density(A) < density(B)`
   - moving **sideways**: allowed into void or into a fluid lighter than A (liquids), or into
     another gas with the mixing probability (gases; see below)

   → **swap** A and B, together with their temperatures (and masses from M4).

Static cells are never displaced. Cells beyond the world edge count as static.

Density comparisons in the movement pass use a pre-quantised integer table
(`g_elem_density_q`). From M3, gas buoyancy uses temperature-adjusted density (see Heat →
Convection).

### Liquids

```
update_liquid(x, y):
    if can_enter(down):                      fall
    else if can_enter(down-left/down-right): move diagonally (random order when both are free)
    else:                                    spread sideways up to DISPERSION[e] cells in the
                                             current flow direction (aux bit), stopping at the
                                             first cell it cannot enter; flip the direction when
                                             blocked
```

- **Fall speed.** M1 may start at 1 px/tick. The stretch goal is to accumulate a vertical velocity
  in `aux` (fixed point, adding `g` each tick while airborne), fall that many cells (stopping at
  the first obstacle), and reset the velocity on landing. It is always clamped to `MAX_MOVE` = 32.
- **Dispersion** (water ≈ 5) controls how fast a liquid levels out. It is clamped to `MAX_MOVE`.
- **Known limitations of naive falling sand**, accepted for M1:
  - Wide basins level slowly.
  - There is no hydrostatic pressure, so a U-tube does not equalise.
  - Revisit in M4 (liquid pressure) if the design needs either.

### Gases (M1 particle model)

One gas particle per pixel. A gas cell tries the following, in order:

1. **Buoyancy against other fluids.** If the cell above is a fluid that is denser than this gas →
   swap with probability `p_buoy = clamp(K_BUOY * (ρ_above - ρ_self) / (ρ_above + ρ_self))`.
   The cell below being a lighter fluid is handled symmetrically by that cell's own update.
2. **Random walk.** Pick one of the 8 neighbours using the chunk RNG.
   - If it is **void** → move there. This is expansion. There is **no gravity bias against void**:
     with one particle per pixel, a gas that "fell" into vacuum would look like powder.
   - If it is **another gas** → swap with a small mixing probability `P_GAS_MIX`, which is higher
     when the swap moves the denser gas downward. This gives partial mixing on top of
     stratification.
   - If it is the same gas → nothing (the swap would be invisible). Mark nothing dirty.
3. Gases never displace liquids sideways or downward; liquids sink through gas by their own
   update.

Expected emergent results: CO2 (1.98 kg/m³) pools under O2 (1.43 kg/m³), H2 (0.09 kg/m³)
rises to the ceiling, and water rains down through all of them. All constants (`K_BUOY`,
`P_GAS_MIX`) are tunable in `sim/rules.c` and recorded in `ELEMENTS.md` when they change.

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

- **Void has k = 0.** Vacuum insulates. Radiation may come later.
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

- RNG: a small fast generator (PCG32 or xorshift-family) per chunk job, seeded with
  `splitmix64(world.seed ^ tick ^ (cx << 32 | cy))`.
- Use it for: random direction choices, swap probabilities, and the `shade` of new cells.
- Never use `rand()`. Never share an RNG between threads. Never let the outcome depend on thread
  count (see `ARCHITECTURE.md` → Determinism).

## Conservation checks (tests)

- Movement never creates or destroys cells: per-element counts stay constant without edits or
  phase changes.
- Without boundary losses, total thermal energy `Σ C_i·T_i` stays constant within float tolerance
  (M3).
- Gas mass is conserved (M4).
