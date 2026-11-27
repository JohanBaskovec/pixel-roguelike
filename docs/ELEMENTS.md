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
| 4 | Ice | STATIC | 917 | 2100 | 2.2 | melts 273.15 K → water, L = 334 kJ/kg; painted as Water below 0 °C | `#BFE6F5` | 1 |
| 5 | Water | LIQUID | 1000 | 4186 | 0.6 | freezes 273.15 K → ice; boils 373.15 K → steam, L = 2257 kJ/kg | `#2A5DB0` α 0.85 | 1 |
| 6 | Oxygen (O₂) | GAS | 1.429 | 918 | 0.026 | condenses 90.19 K → liquid oxygen | `#9CC8F0` α 0.43 | 1 |
| 7 | Carbon dioxide (CO₂) | GAS | 1.977 | 844 | 0.0166 | deposits 194.7 K → dry ice | `#7A7A7A` α 0.55 | 1 |
| 8 | Hydrogen (H₂) | GAS | 0.0899 | 14300 | 0.18 | condenses 20.28 K → liquid hydrogen | `#E9A6E0` α 0.43 | 1 |
| 9 | Water vapour (steam) | GAS | 0.59 (at 100 °C) | 2080 | 0.025 | condenses at its dew point → water | `#E6ECF2` α 0.25 | 3 |
| 10 | Nitrogen (N₂) | GAS | 1.2506 | 1040 | 0.026 | condenses 77.36 K → liquid nitrogen | `#7C86A8` α 0.16 | 4 |
| 11 | Body (a creature) | STATIC | 985 | 3500 | 0.5 | none (held at 37 °C) | `#D9A066` | 4c |
| 12 | Sand | POWDER | 1600 (bulk, dry) | 830 | 0.25 | (melts ≈ 1990 K, later) | `#C2B280` | 5 |
| 13 | Loose soil (dug dirt) | POWDER | 1200 (bulk) | 800 | 0.3 | none | `#7E5A3A` | 5 |
| 14 | Rubble (dug rock) | POWDER | 1700 (bulk, broken rock) | 790 | 0.5 | none | `#85858B` | 5 |
| 15 | Lava (molten rock) | LIQUID | 2600 | 1100 | 1.5 | freezes 1500 K → rock; boils 3000 K → rock vapour, L = 6 MJ/kg | `#C8461E`, glows | 5 |
| 16 | Molten iron | LIQUID | 6980 | 820 | 35 | freezes 1811 K → iron; painted at 1900 K | `#D07830`, glows | 5 |
| 17 | Oil (crude) | LIQUID | 870 | 2000 | 0.15 | none yet (flammable later) | `#3C2A12` α 0.92 | 5 |
| 18 | Bullet (lead) | POWDER | 11340 | 129 | 35 | melts 600.6 K → molten lead, L = 23 kJ/kg | `#50545C` | 6a |
| 19 | Casing (brass) | POWDER | 8500 | 380 | 110 | melts 1208 K → molten brass, L = 168 kJ/kg | `#C9A23A` | 6a |
| 20 | Lead | STATIC | 11340 | 129 | 35 | melts 600.6 K → molten lead | `#5A5E66` | 6a |
| 21 | Molten lead | LIQUID | 10660 | 142 | 16 | freezes 600.6 K → lead; painted at 700 K | `#A8AEB8` | 6a |
| 22 | Brass | STATIC | 8500 | 380 | 110 | melts 1208 K → molten brass | `#B8923A` | 6a |
| 23 | Molten brass | LIQUID | 7600 (approx.) | 470 (approx.) | 70 (approx.) | freezes 1208 K → brass; painted at 1300 K | `#D8A848`, glows | 6a |
| 24 | Gas (a mixture of species 6–10) | GAS | its mixture's (air: 1.2922) | its mixture's (air: 1005) | its mixture's (air: 0.026) | its water vapour condenses | mixed from its species | 6a |
| 25 | Blood | LIQUID | 1060 | 3600 | 0.5 | none (no freezing or boiling yet); starts at 293.15 K | `#8A0F14` α 0.92 | 6b |
| 29 | Walker corpse | STATIC | 985 | 3500 | 0.5 | none; part of a rigid body | `#2C4A2A` | 6b |
| 30 | Flyer corpse | STATIC | 985 | 3500 | 0.5 | none; part of a rigid body | `#4A2C55` | 6b |
| 31 | Liquid oxygen | LIQUID | 1141 | 1700 | 0.15 | freezes 54.36 K, L = 13.9 kJ/kg; boils 90.19 K, L = 213 kJ/kg | `#8CC4F4` α 0.75 | 6b |
| 32 | Solid oxygen | STATIC | 1300 | 1440 | 0.2 | melts 54.36 K | `#6FA8E8` | 6b |
| 33 | Liquid nitrogen | LIQUID | 808 | 2040 | 0.14 | freezes 63.15 K, L = 25.7 kJ/kg; boils 77.36 K, L = 199 kJ/kg | `#C4D2F0` α 0.67 | 6b |
| 34 | Solid nitrogen | STATIC | 947 | 1640 | 0.2 | melts 63.15 K | `#DCE4F4` | 6b |
| 35 | Liquid hydrogen | LIQUID | 70.8 | 9700 | 0.1 | freezes 13.99 K, L = 58 kJ/kg; boils 20.28 K, L = 446 kJ/kg | `#F0C8EC` α 0.63 | 6b |
| 36 | Solid hydrogen | STATIC | 86 | 2800 | 0.9 | melts 13.99 K | `#F6DDF3` | 6b |
| 37 | Dry ice (solid CO₂) | POWDER | 1562 | 1200 | 0.4 | sublimes 194.7 K, L = 571 kJ/kg | `#E4E8EA` | 6b |
| 38 | Rock vapour | GAS | 1.79 (at 0 °C, as SiO) | 1000 | 0.1 | condenses at its dew point → lava | `#E8A060` α 0.59 | 6b |
| 28 | Sawdust | POWDER | 250 (bulk) | 1700 | 0.06 | none (does not burn yet) | `#C4A06A` | 6b |
| 26 | Wood | STATIC | 600 | 1700 | 0.15 | catches fire at 523 K (250 °C) → burning wood | `#9A6A3A` | 6b |
| 27 | Burning wood | STATIC | 600 | 1700 | 0.15 | goes out below 480 K → wood; burns while oxygen reaches it; painted at 900 K (not in the toolbar) | `#4A2A14`, glows | 6b |

Notes
- **Dirt** is a bulk soil value. Real soil ranges from 1200 to 1700 kg/m³, and k from 0.25 (dry) to
  2 (wet).
- **Gas colours** are a deliberate deviation, since real gases are invisible. A gas cell's colour
  mixes its species' colours, each weighted by its α (the blend of the species alone at 1 atm over
  the void background) times its mole fraction; the cell's opacity is that weighted sum times
  √(p / 1 atm), between 0.3× and 2.5× (`GAS_ALPHA_MIN`, `GAS_ALPHA_MAX` in
  `src/render/render.c`), so thin gas stays faintly visible. Nitrogen is the faintest, so air
  looks like a light haze of oxygen, and a mixture fades smoothly as it thins.
- **Per-pixel variation:** every element's brightness varies by up to ± its `shade_var` (0–255
  scale; void 3, dirt 18, rock 14, iron 8, ice 10, water 10, gases 30), so materials look
  textured.
- **Gas mixtures** (D-035): the gases (IDs 6–10) are **species**: a gas cell is `ELEM_GAS` (24)
  and holds a mass of each (a `GasMix`); no cell holds a species element. Pressure
  `P = Σ(m_s/M_s)·R·T/V`. Molar masses: O₂ 0.031999, CO₂ 0.04401, H₂ 0.002016, H₂O 0.018015,
  N₂ 0.028014 kg/mol. A cell of air at 1 atm and 20 °C holds 0.15 g. A cell's heat capacity is
  `Σ m_s · c_s`; its conductivity the mole-fraction-weighted mean of its species'; its density
  (for buoyancy) its real one at 1 atm: air (28.97 g/mol) is lighter than oxygen, heavier than
  nitrogen. The densities in the table are at 0 °C and 1 atm (water vapour: 100 °C). `ELEM_GAS`'s
  row stands for air in the tables (against liquids).
- **Nitrogen** (M4) is inert. With it, air is realistic: the Air brush paints every cell with the
  same mixture, 20.95% O₂ (`AIR_O2_FRAC`), 0.04% CO₂ (`AIR_CO2_FRAC`), the rest N₂ (78.1% N₂ and
  0.9% argon) by moles, at 1 atm, dry. It is settled at once. Breathing (M4c) consumes ~250 mL/min
  of O₂ at rest.
- **Water vapour** (M6a): condenses at its dew point (saturation by Antoine's equation, at 1 atm
  total pressure), so air can be humid. On matter below its dew point it condenses by film
  condensation, `CONDENSE_FILM_H` = 8000 W/(m²K) (the real value for steam, not sped up: D-056).
- **Every gas condenses** (D-057): oxygen, nitrogen and hydrogen to their liquids, CO₂ to dry ice,
  rock vapour to lava, each at its dew point for its share of the gas (at 1 atm total pressure,
  as water). Saturation pressure: water by Antoine's equation, the others by Clausius-Clapeyron
  from the boiling point and latent heat (one curve above the liquid and the solid alike, an
  approximation). There is no liquid CO₂: at 1 atm it does not exist (it needs 5.1 atm, and phase
  changes do not depend on pressure yet). Dry ice is a powder, so the frost that forms in cold air
  falls rather than hangs.
- **Rock vapour** is a stand-in for boiled silicate rock: molar mass of SiO (44 g/mol), boiling at
  3000 K at 1 atm with 6 MJ/kg (real silicates vaporise between about 2500 and 3200 K, partly
  decomposing). Iron does not boil yet (3134 K: it needs a gas slot, ROADMAP).
- **Painting** (D-057): a tool paints at the temperature chosen in the game (20 °C to start) and
  the substance takes its form at it at 1 atm (`elem_form_at`). An edit with no temperature uses
  the element's spawn temperature (the table's), as tests and scenes do: 293.15 K for most, ice
  263.15 K, steam 383.15 K, lava 1600 K, the cryogenic liquids just below their boiling points.
- **Phase changes** (M3): ice → water above 0 °C (L = 334 kJ/kg); water → ice below 0 °C; water →
  water vapour above 100 °C (L = 2257 kJ/kg); vapour → water at its dew point (M6a). **Lead and brass** (M6a): spent
  bullets and solid lead melt at 327.5 °C; casings and solid brass at 935 °C (cartridge brass,
  70% copper and 30% zinc, melts between about 915 and 955 °C; one point is used). Molten metal
  solidifies into solid lead or brass, not back into bullets or casings. Molten brass's density,
  heat capacity and conductivity are estimates from copper's and zinc's; brass fuming off its zinc
  when molten is not modelled. Tables `g_elem_above` /
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
For gases, the spread (how far a split-off half of dense gas, or thin gas as a particle, moves
into void per tick) follows the mixture: 3 × √(29 g/mol / molar mass), between 1 and 6: air 3,
CO₂ 2, water vapour 4, hydrogen 6.

All are clamped to `MAX_MOVE` = 32.

| Global | Value | Where | Meaning |
|---|---|---|---|
| `K_BUOY` (`K_BUOY_16` = 65536) | 1.0 | `sim/elements.h` | Scales the buoyancy swap probability between fluids |
| `GAS_DIFFUSE_K` | 3.3% | `sim/sim.c` | Diffusion: a pair trades this share of each species' mole-fraction difference (times their mean moles) when picked, about every other tick: 2.5e-3 m²/s (deviation 12) |
| `GAS_MIX_TOL`, `GAS_BUOY_TOL` | 0.1%, 0.02% | `sim/sim.c` | Mole fractions, and densities at 1 atm, closer than this count as equal (no move; may sleep) |
| `GAS_BUOY_SCALES` | 1, 4, 8, 16 rows | `sim/sim.c` | Buoyancy: column windows compared above and below; drive = contrast × √height |
| `GAS_SPREAD_H`, spread distances | 8 rows; 1, 4, 8, 16 cells | `sim/sim.c` | Heavy gas spreading: column weight compared with columns this far to the side |
| `GAS_MIN_FRAC` | 0.01 | `sim/elements.h` | Smallest cell a split may leave, as a fraction of the moles of 1 atm; thinner gas moves as a particle |
| `GAS_REACH` | 16 | `sim/sim.c` | Farthest gas cell a cell equalises pressure with in one tick (the full reach half the time) |
| `GAS_JUMP_MAX_DT` | 2 K | `sim/sim.c` | Long equalisation jumps only between cells this close in temperature |
| `GAS_EQ_TOL`, `GAS_EQ_TOL_FAR` | 0.3%, 0.2% | `sim/sim.c` | Pressures closer than this count as equal (no transfer; may sleep): for cells under 8 apart, and further |
| `GAS_FEED_RATIO` | 2.1 | `sim/sim.c` | Thin gas drifting in a vacuum is not fed by gas this many times denser (dense gas splits into the vacuum instead) |
| `GAS_REGION_CELLS` | 256 | `sim/world.h` | Cells of gas a compression by a liquid or a landing particle is spread over (within `GAS_REGION_RADIUS` = 16) |
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
| `WIND_DECAY_P16`, `WIND_SPREAD_P16` | 1/4, 1/2 | `sim/sim.c` | Gas currents: chance per tick to lose a speed step, to spread to a neighbour |
| `WIND_PASS_LOSS`, `WIND_SPREAD_LOSS`, `WIND_TURN_LOSS` | 2, 4, 3 | `sim/sim.c` | Speed a current loses when it pushes a cell, spreads, or turns at a wall |
| `BODY_PUSH_WIND`, `BODY_WAKE_WIND`, `BREATH_WIND` | 12, 18, 14 | `sim/creature.c` | Currents from a walking body (front, wake) and a breath out |
| `PLAYER_W`, `PLAYER_H`, `PLAYER_SPEED` | 5, 17 cells, 4 m/s | `sim/creature.h` | The player: 25 cm × 85 cm (half size, D-038); running speed |
| `CREATURE_JUMP_SPEED` | 3.13 m/s | `sim/creature.h` | Take-off speed of a jump (about 50 cm high) |
| `AIR_CONTROL` | 10% of walking speed per tick | `sim/creature.c` | Steering in the air (deviation 15): full speed in 0.17 s |
| `LIFT_SIDE` | 0.6 cells/tick | `sim/creature.c` | Powder in mid-air a body moves into slides off it this fast as it falls |
| `FLOW_SIDE` | 8 columns | `sim/creature.c` | How far beside a body moving up or down its displaced liquid goes to the surface |
| `PROPELLANT_KG`, `MUZZLE_K` | 1.6 g, 1500 K | `sim/weapon.c` | Muzzle gas per shot (55% CO₂, 27% water vapour, 18% N₂ by mass) and its temperature |
| `MUZZLE_REACH` | 6 cells | `sim/weapon.c` | Muzzle gas is mixed into the gas of this many cells along the line of fire |
| `GAS_TURB_PER_WIND` | 23 | `sim/world.h` | Turbulence: heat exchange and diffusion between gas cells × (1 + this × current speed) |
| `RIFLE_BULLET_SPEED` | 900 m/s (300 cells/tick) | `sim/weapon.h` | Muzzle velocity |
| `RIFLE_INTERVAL` | 5 ticks | `sim/weapon.h` | Between shots: 720 rounds per minute |
| `RIFLE_SPREAD` | ±0.005 rad | `sim/weapon.h` | Random spread of a shot (about ±0.3°) |
| `BULLET_FAST` | 10 cells/tick (30 m/s) | `sim/particle.c` | Below this a bullet just falls (an ordinary particle): spent bullets kicked or knocked out never dig |
| `BULLET_WIND` | 8 | `sim/particle.c` | Current a bullet leaves in the gas along its path |
| bullet resistance | dirt and powders 50, liquids 15 × ρ/1000 cells/tick per cell; rock: chips one cell and stops | `sim/particle.c` | Speed a bullet loses per cell (it flies at 300): about 30 cm of earth or sand, 1 m of water |
| `CLUMP_MAX_CELLS` | 20 | `sim/edit.c` | Solid clumps this small, held by nothing, break loose |
| `KICK_ROWS` | 4 cells (20 cm) | `sim/creature.c` | Powder piles at most this high in a walker's path are kicked ahead |
| `KICK_AHEAD` | 12 cells | `sim/creature.c` | Not kicked if powder rises higher within this many columns (a heap: climbed) |
| `FALL_WIND_BASE` | 6 | `sim/sim.c` | Current from a falling stream (plus fall speed / 4) |
| `FALL_WIND_LONE` | 4 | `sim/sim.c` | Current a lone falling cell gives the gas it displaces (never spreads, dies in about 16 ticks) |
| Reaction H₂ + O₂ | 773 K, 7.94 kg O₂/kg, 120 MJ/kg | `sim/elements.c` | Hydrogen burning into steam (`g_reactions`) |
| `FLAME_MAX_K` | 3000 K | `sim/elements.h` | Hottest a reaction product gets |
| `POWDER_SINK_P16` | 3000/65536 | `sim/sim.c` | Chance per tick that a powder sinks one cell through liquid (≈ 0.15 m/s) |
| `CREATURE_WALK_SPEED` | 1.4 m/s | `sim/creature.h` | Walking speed (half when impaired or in liquid) |
| `STEP_UP` | 2 cells | `sim/creature.c` | Highest ledge (10 cm) walked up without stopping |
| `BREATH_TICKS`, `INHALE_TICKS` | 257, 103 | `sim/creature.c` | One breath every 4.3 s (14 a minute); breathing in takes 1.7 s, then out |
| `TIDAL_CELLS` | 4 cells (0.5 L) | `sim/creature.c` | Air breathed in and out per breath at rest (×3 walking) |
| `DRAW_MIN`, `DRAW_MAX_FRAC` | 32 cells, 1/2 | `sim/creature.c` | A breath draws from at least the 32 gas cells nearest the mouth, at most half of each |
| `O2_EXTRACT_MAX` | 1/2 | `sim/creature.c` | Most of the oxygen in the lungs the body takes per breath |
| `EXHALE_K` | 307 K (34 °C) | `sim/creature.c` | Temperature of the breath out |
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
| `RAD_TIME_SCALE` | 30 | `sim/elements.h` | Speed-up of radiation (reality's, at `HEAT_TIME_SCALE`, would freeze a thin lava film within a second) |
| `RAD_MIN_K`, `RAD_EMISSIVITY`, `RAD_RANGE` | 600 K, 0.9, 32 cells | `sim/elements.h` | Radiation starts at 600 K; emissivity; how far a ray goes |
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
3. A gas cell holds a mixture (D-035; it was one gas per cell, D-005). Pressure spreads by
   diffusion (a few seconds across a room), not at the speed of sound, and evens out to within
   0.3% between neighbours and 0.2% per 16 cells of path (`GAS_EQ_TOL`, `GAS_EQ_TOL_FAR`).
   Condensing vapour becomes a full water cell with probability m / m_water (mass conserved on
   average), at its dew point taken at 1 atm total pressure. Water does not evaporate below 100 °C.
4. Liquids are incompressible. Pressure moves water from the top of a body straight to the
   opening it is pushed toward, and the flow rate through an opening is not proportional to the
   square root of the depth (see SIMULATION.md → Liquid pressure).
5. No heat radiation: vacuum is a perfect insulator (D-017).
6. Gas pressure pushes on liquids, but gas-driven flow is rate limited (`GAS_LIQUID_INTERVAL`),
   so a barometer fills in about 20 s rather than at once.
7. Buoyancy between gases compares column windows (1 to 16 rows) and moves a cell or shifts a
   column at a time: a coarse stand-in for flow (a 50 cm pocket 30 K warmer than the room rises at
   about 0.2 m/s, slower than a real one). A gradient gentler than the tolerances is left alone.
8. Reactions (M5): products are not heated beyond `FLAME_MAX_K` (3000 K), since dissociation is
   not modelled; the rest of the heat is dropped. All the fuel the oxidiser in a cell allows burns
   in one tick.
9. Lava and molten iron flow like water under water pressure; only their sideways spread is
   smaller (viscosity is not modelled). Rock melts at a single temperature (real rock melts over
   a range, about 1300–1500 K for granite).
10. Gas currents are a coarse stand-in for air flow: a direction and a speed per gas cell, not a
    solved velocity field (no vortices from pressure, no inertia beyond the current fading over
    about 2 s). They are made by bodies and falling matter, not by pressure differences. A current
    in gas exactly like the gas around it stops at once (it would change nothing there; a clod
    falling through a clear sky would otherwise leave a churning trail for seconds), so a gust
    does not carry across clean air into smoke beyond. A lone falling cell kicks only the gas it
    displaces (`FALL_WIND_LONE`); only streams drag the air beside them.
11. Creatures (M4c): each breath in also stirs the air around a person, swapping 2 L worth of gas cells
   near the mouth with gas up to 3.2 m away (`STIR_CELLS`, `STIR_FAR`); real rooms mix by air
   currents, which the game's diffusion is too slow for when breathing is sped up. Body heat is held at 37 °C with up to
   100 W, scaled by the gas conduction speed-up (×10) like the heat it loses. The body is a
   5 × 17 rectangle: half a person's size (D-038), with a person's breathing, body heat, running
   speed (4 m/s) and jump (50 cm); its rifle is 45 cm long. `metabolism_scale` speeds up breathing and health for gameplay or tests (1:
   real time).
12. Gas mixtures (D-035): still gas diffuses down each species' mole-fraction difference at about
    2.5e-3 m²/s, about 150× real molecular diffusion (`GAS_DIFFUSE_K`): indoor air is never
    perfectly still, and nothing should stay an unmixed clump. Stirred gas mixes faster still
    (`GAS_TURB_PER_WIND`), standing in for turbulence. Argon is counted as nitrogen.
13. Weapons (M6a): a bullet is one cell (5 cm), far bigger than a real 5.56 mm bullet; its
    penetration (about 30 cm of earth, one chip of rock) is a rough figure, not a ballistic model,
    and it does not ricochet. Spent bullets and casings are full cells of lead and brass at their
    solid densities (a real casing is hollow). Bullets and casings start at room temperature,
    though real ones leave at about 100 °C: a real bullet then holds about 50 J of heat, a cell of
    lead at 400 K 19 kJ, which would heat a trail of air all the way down as it falls. The rifle
    never runs out of ammunition. Muzzle
    velocity is real (900 m/s): a bullet crosses the screen in a few ticks, so the game draws a
    faint streak behind it (rendering only). The muzzle gas has no carbon monoxide or hydrogen
    (counted as CO₂ and water vapour), so it does not burn in a secondary flash; a gas cell with a
    current exchanges heat with its gas neighbours by turbulence (`GAS_TURB_PER_WIND`, an eddy
    diffusivity of one cell times the current's speed), which is what cools it within a few
    ticks.
14. Breathing (M6a): a breath draws the same fraction of every gas cell near the mouth, standing
    in for the mixed air the mouth draws in; the lungs hold only the tidal volume (real lungs keep
    about 2.5 L more), so a held breath lasts a few breaths, not a minute or two. The breath out
    is dry.
15. Air control (M6a): the player steers in the air (`AIR_CONTROL`), which a real jumper cannot.
16. Simulated area (D-040, D-053): only the area around the player (400 columns and 224 rows either
    side) is simulated. Outside, time stops: a lava pool stops cooling, gas stops spreading, a
    creature stands still, until the player comes back. Heat does not cross into the frozen part,
    and gas and liquids stop at its edge as at a wall that moves with the player; free particles
    (bullets, debris, splashes) keep flying and land there.
17. Jetpack (M6b, D-041): the player has an infinite jetpack, a game invention. Held for 12 ticks
    (0.2 s) in the air, `W` adds 2.2 g of upward acceleration (1.2 g net of gravity) up to 5 m/s
    of rise (`JETPACK_ACCEL`, `JETPACK_MAX_RISE`). It exerts no force on the air yet.
18. Monsters (M6b, D-042): they do not breathe and give off no body heat, and their bodies start
    at room temperature (a 310 K body in 293 K air keeps its chunks in the heat pass for minutes:
    50 monsters cost 12 ms a tick). Their size (6 × 6 and 11 × 11 cells), speed, hit points (160 and 120)
    and
    bite are game values (`WALKER_*`, `FLYER_*`, `RIFLE_DAMAGE` 12 hp, `BITE_TICKS`). Blood's
    droplets start at room temperature too, like bullets (the heat they carry is negligible).
19. Burning wood (M6b, D-044): the chemistry is real (cellulose C₆H₁₀O₅ + 6 O₂ → 6 CO₂ + 5 H₂O,
    1.185 kg of O₂ per kg of wood, 16 MJ/kg, a fire needs 16% oxygen by volume), but a wood cell
    is not a 5 cm cube of wood to the fire: in this 5 cm slice, burning one would take the oxygen
    of 2400 cells of air, and a room of air could hardly burn a log. So a cell holds
    `WOOD_FUEL_SCALE` = 2% of the fuel of its mass (1.5 g: O₂, CO₂ and H₂O are scaled with it) and
    burns in `WOOD_BURN_TICKS` = 240 ticks (4 s) at full rate; its heat is `WOOD_HEAT_SCALE` = 10×
    too high for that fuel (what keeps the fire going: wood beside it is heated by conduction at 1/1000 of
    a real second per tick, and by flame contact, `WOOD_SPREAD` = 4× conduction, up to ignition).
    Oxygen for the burn is drawn from the 256 gas cells nearest the flame (within 16 cells), in
    proportion, if their mean oxygen is at least 16% by volume; the products enter the nearest
    gas cell, hot. Heat released: `WOOD_FUEL_HEAT` 70% into the wood (the rest into that gas cell,
    up to `WOOD_FLAME_K` 1400 K, and the excess into the wood). A cell burns away without ash.
    Heat between wood and gas is `WOOD_GAS_COUPLING` = 100× what the gas conducts (flames and hot
    gas also radiate and convect; still air conducts to wood 100× too slowly for a fire), and an
    ember (a burning cell with no oxygen around it) still heats the wood beside it. Plain
    wood-to-wood conduction is real: wood is an insulator, so heat crosses a few cells of it
    in minutes (at the 1000× solid time scale).

## Adding an element (checklist)

1. Add a row to the table above, with sourced reference values and the milestone.
2. Add the entry to the definition table in `src/sim/elements.c`. Keep IDs stable, because saves
   (M6) will depend on them.
3. If it is paintable, add it to the toolbar in `src/game/`.
4. Add behaviour tests for anything new (a new behaviour, a phase change, a reaction).
   A new **gas** is a species, not a cell element of its own: give it an element row for its
   properties, then add it to `GAS_SPECIES`, the `GAS_*` indices and `g_species_elem`
   (`sim/elements.h`, `sim/elements.c`) with its molar mass, and what it condenses to in
   `g_species_condensed` (that element's `g_elem_above` must boil into `ELEM_GAS`). Every gas
   cell then carries it (4 more bytes per cell, about 5% more time for gas: PERFORMANCE).
5. Update "Gameplay deviations" if anything is non-physical.
