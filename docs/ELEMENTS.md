# Elements

The reference for every element. `src/sim/elements.c` holds one definition table that generates
the lookup arrays. **This doc and that table must agree.** Update both in the same commit.

Values are real-world references at about 1 atm, with gases at 0 °C (STP). Where the game deviates,
the deviation is listed under "Gameplay deviations" below. Never change it silently in code.

## Element table

| ID | Element | Behaviour | Density (kg/m³) | Heat capacity c (J/kg·K) | Conductivity k (W/m·K) | Transitions (1 atm) | Base colour | M |
|---|---|---|---|---|---|---|---|---|
| 0 | Void | NONE | 0 | — | 0 | — | `#0B0B10` | 1 |
| 1 | Dirt | STATIC | 1500 | 800 | 0.5 | — (later: dries or bakes) | `#6B4A2E` | 1 |
| 2 | Rock (granite) | STATIC | 2700 | 790 | 2.8 | melts 1500 K → lava, L = 400 kJ/kg | `#6E6E73` | 1 |
| 3 | Iron | STATIC | 7874 | 449 | 80 | melts 1811 K → molten iron, L = 247 kJ/kg | `#8A8F96` | 1 |
| 4 | Ice | STATIC | 917 | 2100 | 2.2 | melts 273.15 K → water, L = 334 kJ/kg | `#BFE6F5` | 1 |
| 5 | Water | LIQUID | 1000 | 4186 | 0.6 | freezes 273.15 K → ice; boils 373.15 K → steam, L = 2257 kJ/kg | `#2A5DB0` α 0.85 | 1 |
| 6 | Oxygen (O₂) | GAS | 1.429 | 918 | 0.026 | condenses 90.2 K (liquid O₂, later) | `#9CC8F0` α 0.43 | 1 |
| 7 | Carbon dioxide (CO₂) | GAS | 1.977 | 844 | 0.0166 | deposits 194.7 K → dry ice (later) | `#7A7A7A` α 0.55 | 1 |
| 8 | Hydrogen (H₂) | GAS | 0.0899 | 14300 | 0.18 | condenses 20.3 K (later) | `#E9A6E0` α 0.43 | 1 |
| 9 | Steam | GAS | 0.59 (at 100 °C) | 2080 | 0.025 | condenses 373.15 K → water | `#E6ECF2` α 0.25 | 3 |
| 10 | Nitrogen (N₂) | GAS | 1.2506 | 1040 | 0.026 | condenses 77.4 K (later) | `#7C86A8` α 0.16 | 4 |
| 11 | Body (a creature) | STATIC | 985 | 3500 | 0.5 | none (held at 37 °C) | `#D9A066` | 4c |
| 12 | Sand | POWDER | 1600 (bulk, dry) | 830 | 0.25 | (melts ≈ 1990 K, later) | `#C2B280` | 5 |
| 13 | Loose soil (dug dirt) | POWDER | 1200 (bulk) | 800 | 0.3 | none | `#7E5A3A` | 5 |
| 14 | Rubble (dug rock) | POWDER | 1700 (bulk, broken rock) | 790 | 0.5 | none | `#85858B` | 5 |
| 15 | Lava (molten rock) | LIQUID | 2600 | 1100 | 1.5 | freezes 1500 K → rock; painted at 1600 K | `#C8461E`, glows | 5 |
| 16 | Molten iron | LIQUID | 6980 | 820 | 35 | freezes 1811 K → iron; painted at 1900 K | `#D07830`, glows | 5 |
| 17 | Oil (crude) | LIQUID | 870 | 2000 | 0.15 | none yet (flammable later) | `#3C2A12` α 0.92 | 5 |

Notes
- **Dirt** is a bulk soil value. Real soil ranges from 1200 to 1700 kg/m³, and k from 0.25 (dry) to
  2 (wet).
- **Gas colours** are a deliberate deviation, since real gases are invisible. The α is the blend
  of one gas pixel at 1 atm over the void background. From M4 it scales with density:
  α · √(m / m_1atm), between 0.3× and 2.5× (`GAS_ALPHA_MIN`, `GAS_ALPHA_MAX` in
  `src/render/render.c`), so thin gas stays faintly visible. Nitrogen is the faintest, so air
  looks like a light haze of oxygen.
- **Per-pixel variation:** every element's brightness varies by up to ± its `shade_var` (0–255
  scale; void 3, dirt 18, rock 14, iron 8, ice 10, water 10, gases 30), so materials look
  textured.
- **Gas mass and pressure** (M4): a gas cell holds a mass; pressure `P = (m/M)·R·T/V`. Molar
  masses: O₂ 0.031999, CO₂ 0.04401, H₂ 0.002016, H₂O 0.018015, N₂ 0.028014 kg/mol. A cell of air at
  1 atm and 20 °C holds 0.15 g. The densities in the table are at 0 °C and 1 atm (steam: 100 °C).
- **Buoyancy** treats O₂ and N₂ as air: both get `AIR_DENSITY_0C` = 1.2922 kg/m³ for buoyancy only
  (deviation 7 below).
- **Nitrogen** (M4) is inert. With it, air can be realistic: 78% N₂, 21% O₂, ~1% argon (ignored),
  0.04% CO₂ by volume. The Air brush paints each cell oxygen with probability `AIR_O2_FRAC` = 0.21,
  else nitrogen (argon counted as nitrogen). Breathing (M4c) consumes ~250 mL/min of O₂ at rest.
- **Default spawn temperature** for all painted elements is 293.15 K (20 °C). Exceptions: ice
  spawns at 263.15 K and steam at 383.15 K, so they do not change phase immediately.
- **Phase changes** (M3): ice → water above 0 °C (L = 334 kJ/kg); water → ice below 0 °C; water →
  steam above 100 °C (L = 2257 kJ/kg); steam → water below 100 °C. Tables `g_elem_above` /
  `g_elem_below` in `src/sim/elements.c`. Water's density follows its real curve (densest at
  4 °C); gases follow the ideal gas law.

## Derived values at 5 cm cells

These are handy for sanity-checking (`V_cell = 1.25e-4 m³`).

| Element | Mass per cell | Thermal mass per cell (J/K) | Diffusivity α = k/(ρc) (m²/s) |
|---|---|---|---|
| Water | 0.125 kg | 523 | 1.4 × 10⁻⁷ |
| Iron | 0.984 kg | 442 | 2.3 × 10⁻⁵ |
| Rock | 0.338 kg | 267 | 1.3 × 10⁻⁶ |
| Oxygen | 0.18 g | 0.164 | 2.0 × 10⁻⁵ |
| Hydrogen | 0.011 g | 0.161 | 1.4 × 10⁻⁴ |

A gas cell has about 3000× less thermal mass than a water cell. That is why the heat pass needs the
pairwise flow limiter (`SIMULATION.md` → Stability).

## Simulation tuning constants

Movement constants. These are game-feel values, not physics.

| Element | `spread` (cells/tick) | Meaning |
|---|---|---|
| Water | 5 | Sideways dispersion. Landing speed adds to it for one tick |
| Oxygen | 3 | Random-walk step length into void |
| CO₂ | 2 | Heavier molecules, slower spread |
| Hydrogen | 6 | Lighter molecules, faster spread |
| Steam | 4 | Water vapour (18 g/mol), between oxygen and hydrogen |
| Nitrogen | 3 | Like oxygen |

For gases, `spread` is how far a split-off half (dense gas) or a particle (thin gas) moves into
void.

All are clamped to `MAX_MOVE` = 32.

| Global | Value | Where | Meaning |
|---|---|---|---|
| `K_BUOY` | 1.0 | `sim/elements.h` | Scales the buoyancy swap probability between fluids |
| `P_GAS_MIX` | 1/2 | `sim/elements.h` | Probability that two different gases trade places when that lowers their same-gas contacts |
| `GAS_MIN_FRAC` | 0.01 | `sim/elements.h` | Smallest cell a split may leave, as a fraction of the 1-atm mass; thinner gas moves as particles |
| `GAS_REACH` | 16 | `sim/sim.c` | Farthest cell of the same gas a cell equalises pressure with in one tick |
| `GAS_JUMP_MAX_DT` | 2 K | `sim/sim.c` | Long equalisation jumps only between cells this close in temperature |
| `GAS_EQ_TOL` | 0.1% | `sim/sim.c` | Pressures closer than this count as equal (no transfer; may sleep) |
| `GAS_PUSH_RATIO` | 2.1 | `sim/sim.c` | Pressure ratio above which a gas pushes a neighbouring gas cell aside |
| `GAS_BALANCE_TOL` | 1% | `sim/sim.c` | A gas this far above a different neighbouring gas takes its cell over (spread over both regions) |
| `GAS_BALANCE_P16` | 1/16 | `sim/sim.c` | Chance per tick of trying that (regions of 64 cells within 8) |
| `GAS_REGION_CELLS` | 256 | `sim/world.h` | Cells of gas a compression or expansion by a liquid is spread over (within `GAS_REGION_RADIUS` = 16) |
| `GAS_SAME_REGION` | 5% | `sim/pressure.c` | Gas at an opening this close to the gas above the source just trades places with the liquid |
| `GAS_LIQUID_INTERVAL` | 2 ticks | `sim/pressure.c` | A body may compress or expand gas at most once per this many ticks |
| `PARTICLE_GRAVITY` | 0.0545 cells/tick² | `sim/particle.h` | Free particles' gravity (9.81 m/s²) |
| `PARTICLE_MAX_SPEED` | 24 cells/tick | `sim/particle.h` | Speed limit of free particles (about 30 m/s) |
| `JET_MIN_SPEED` | 1 cell/tick | `sim/pressure.c` | Outflow speed (about 9 cells of depth) from which a side opening jets |
| `JET_MAX_PER_REQUEST` | 8 | `sim/pressure.c` | Most cells one opening cell launches per tick |
| `JET_SPREAD` | 6% | `sim/pressure.c` | Spread across a jet (±3% of its speed) |
| `SPLASH_MIN_SPEED` | 2 cells/tick | `sim/sim.c` | Landing speed from which falling liquid may splash |
| `SPLASH_P16` | 1/4 | `sim/sim.c` | Chance that such a landing splashes |
| `SPLASH_KEEP_MIN/MAX`, `SPLASH_UP` | 25–60%, 25% | `sim/sim.c` | Share of its speed a splash keeps, sideways and upward |
| Reaction H₂ + O₂ | 773 K, 7.94 kg O₂/kg, 120 MJ/kg | `sim/elements.c` | Hydrogen burning into steam (`g_reactions`) |
| `FLAME_MAX_K` | 3000 K | `sim/elements.h` | Hottest a reaction product gets |
| `REACT_REACH` | 3 cells | `sim/sim.c` | Farthest oxidiser cell a fuel cell reacts with |
| `POWDER_SINK_P16` | 3000/65536 | `sim/sim.c` | Chance per tick that a powder sinks one cell through liquid (≈ 0.15 m/s) |
| `WALK_SPEED` | 1.4 m/s | `sim/creature.c` | Walking speed (half when impaired or in liquid) |
| `STEP_UP` | 4 cells | `sim/creature.c` | Highest ledge (20 cm) walked up without stopping |
| `BREATH_TICKS` | 257 | `sim/creature.c` | One breath every 4.3 s (14 a minute) |
| `O2_KG_PER_S`, `CO2_KG_PER_S` | 250 / 200 mL/min | `sim/creature.c` | O₂ used and CO₂ made at rest (0.333 / 0.368 g/min); ×3 walking |
| `PO2_IMPAIRED`, `PO2_DANGER`, `PCO2_DANGER` | 16 / 10 / 5 kPa | `sim/creature.c` | Partial pressures around the head (16% / 10% O₂, 5% CO₂ of an atmosphere) |
| `DANGER_SECONDS`, `RECOVER_SECONDS` | 120 s, 600 s | `sim/creature.c` | Time to die in danger; time to recover fully |
| `BODY_POWER_W` | 100 W | `sim/creature.c` | Body heat at rest (×3 walking, ×10 conduction speed-up) |
| `STIR_CELLS`, `STIR_FAR` | 16 cells, 64 cells | `sim/creature.c` | Air stirred per breath (2 L) and how far off it is exchanged (3.2 m) |
| `GRAVITY` | 9.81 m/s² | `sim/elements.h` | Gas pressure as a liquid height: P / (ρ·g·Δx); 1 atm = 207 cells of water |
| `GRAVITY_INC_P16` | 0.87 | `sim/sim.c` | Chance per tick a falling liquid gains 1/16 px/tick of speed (≈ 9.8 m/s² at 5 cm/px) |
| `FALL_SPEED_MAX` | 127/16 px/tick | `sim/sim.c` | Terminal fall speed, about 8 px/tick (24 m/s) |
| `LIQUID_LOOKAHEAD` | 32 | `sim/sim.c` | How far a surface liquid cell looks sideways for a drop (= `MAX_MOVE`) |
| `TICK_MAX_REQUESTS` | 128 | `sim/sim.c` | Water-pressure transfer requests per chunk per tick |
| `SOURCE_ROWS` | 8 | `sim/pressure.c` | Rows below a body's top a transfer may take water from once the top row is used up |
| `HEAT_TIME_SCALE` | 1000 | `sim/elements.h` | Speed-up of conduction in solids and liquids (see SIMULATION.md) |
| `HEAT_TIME_SCALE_GAS` | 10 | `sim/elements.h` | Speed-up of conduction in any contact involving a gas |
| `HEAT_SLEEP_K` | 0.001 K/tick | `sim/heat.c` | A chunk stays heat-active while temperatures change faster than this |
| `HEAT_WAKE_MOVEMENT_K` | 0.02 K/tick | `sim/heat.c` | Temperature change that wakes a chunk's fluids (convection) |
| `K_CONVECTION` | 200 | `sim/sim.c` | Liquid convection swap probability per unit of relative density difference |
| `CONVECTION_MIN_K` | 0.05 K | `sim/sim.c` | Smallest temperature difference that drives convection or same-gas buoyancy |
| `EDIT_HEAT_STEP_K` | 8 K | `sim/edit.h` | Heat / Cool brush change per application (60 per second while held) |

## Gameplay deviations from reality

Keep this list complete.

1. Gases are tinted so they are visible.
2. Heat conduction is sped up: 1000× in solids and liquids, 10× where a gas is involved
   (`HEAT_TIME_SCALE`, `HEAT_TIME_SCALE_GAS`). Relative conductivities stay real.
3. One gas per cell (D-005): different gases never share a cell, so total pressure evens out
   between two gases cell by cell, to within about 3%. Pressure spreads by
   diffusion (a few seconds across a room), not at the speed of sound. Condensing steam becomes a
   full water cell with probability m / m_water (mass conserved on average), at 100 °C whatever
   its partial pressure.
4. Liquids are incompressible. Pressure moves water from the top of a body straight to the
   opening it is pushed toward, and the flow rate through an opening is not proportional to the
   square root of the depth (see SIMULATION.md → Liquid pressure).
5. No heat radiation: vacuum is a perfect insulator (D-017).
6. Gas pressure pushes on liquids, but gas-driven flow is rate limited (`GAS_LIQUID_INTERVAL`),
   so a barometer fills in about 20 s rather than at once.
7. For buoyancy, oxygen and nitrogen have the same density (air, `AIR_DENSITY_0C`), so painted
   air does not unmix. Real O₂ is 14% denser than N₂, but air stays mixed because it is mixed
   molecule by molecule, which one-gas-per-cell cannot represent.
8. Reactions (M5): products are not heated beyond `FLAME_MAX_K` (3000 K), since dissociation is
   not modelled; the rest of the heat is dropped. Fuel finds its oxidiser up to 3 cells away
   (`REACT_REACH`), standing in for molecular mixing.
9. Lava and molten iron flow like water under water pressure; only their sideways spread is
   smaller (viscosity is not modelled). Rock melts at a single temperature (real rock melts over
   a range, about 1300–1500 K for granite).
10. Creatures (M4c): each breath also stirs the air around a person, swapping 2 L worth of gas cells
   near the mouth with gas up to 3.2 m away (`STIR_CELLS`, `STIR_FAR`); real rooms mix by air
   currents, which one-gas-per-cell diffusion cannot do. Body heat is held at 37 °C with up to
   100 W, scaled by the gas conduction speed-up (×10) like the heat it loses. The body is a
   9 × 35 rectangle. `metabolism_scale` speeds up breathing and health for gameplay or tests (1:
   real time).

## Adding an element (checklist)

1. Add a row to the table above, with sourced reference values and the milestone.
2. Add the entry to the definition table in `src/sim/elements.c`. Keep IDs stable, because saves
   (M6) will depend on them.
3. If it is paintable, add it to the toolbar in `src/game/`.
4. Add behaviour tests for anything new (a new behaviour, a phase change, a reaction).
5. Update "Gameplay deviations" if anything is non-physical.
