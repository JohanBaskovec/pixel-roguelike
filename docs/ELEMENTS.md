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
| 2 | Rock (granite) | STATIC | 2700 | 790 | 2.8 | melts ≈ 1500 K → magma (M5) | `#6E6E73` | 1 |
| 3 | Iron | STATIC | 7874 | 449 | 80 | melts 1811 K → molten iron (M5) | `#8A8F96` | 1 |
| 4 | Ice | STATIC | 917 | 2100 | 2.2 | melts 273.15 K → water, L = 334 kJ/kg | `#BFE6F5` | 1 |
| 5 | Water | LIQUID | 1000 | 4186 | 0.6 | freezes 273.15 K → ice; boils 373.15 K → steam, L = 2257 kJ/kg | `#2A5DB0` α 0.85 | 1 |
| 6 | Oxygen (O₂) | GAS | 1.429 | 918 | 0.026 | condenses 90.2 K (liquid O₂, later) | `#9CC8F0` α 0.43 | 1 |
| 7 | Carbon dioxide (CO₂) | GAS | 1.977 | 844 | 0.0166 | deposits 194.7 K → dry ice (later) | `#7A7A7A` α 0.55 | 1 |
| 8 | Hydrogen (H₂) | GAS | 0.0899 | 14300 | 0.18 | condenses 20.3 K (later) | `#E9A6E0` α 0.43 | 1 |
| 9 | Steam | GAS | 0.59 (at 100 °C) | 2080 | 0.025 | condenses 373.15 K → water | `#E6ECF2` α 0.25 | 3 |
| 10 | Nitrogen (N₂) | GAS | 1.2506 | 1040 | 0.026 | condenses 77.4 K (later) | faint, TBD | 4 |

Notes
- **Dirt** is a bulk soil value. Real soil ranges from 1200 to 1700 kg/m³, and k from 0.25 (dry) to
  2 (wet).
- **Gas colours** are a deliberate deviation, since real gases are invisible. The α is the blend
  of one gas pixel over the void background. It is high enough that a thin cloud of single
  particles is still readable. From M4 it scales with mass.
- **Per-pixel variation:** every element's brightness varies by up to ± its `shade_var` (0–255
  scale; void 3, dirt 18, rock 14, iron 8, ice 10, water 10, gases 30), so materials look
  textured.
- **Molar masses** (needed from M4): O₂ 0.032, CO₂ 0.044, H₂ 0.002, H₂O 0.018, N₂ 0.028 kg/mol.
- **Nitrogen** (M4) is inert. With it, air can be realistic: 78% N₂, 21% O₂, ~1% argon (ignored),
  0.04% CO₂ by volume. Breathing (M4c) consumes ~250 mL/min of O₂ at rest.
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

All are clamped to `MAX_MOVE` = 32.

| Global | Value | Where | Meaning |
|---|---|---|---|
| `K_BUOY` | 1.0 | `sim/elements.h` | Scales the buoyancy swap probability between fluids |
| `P_GAS_MIX` | 1/16 | `sim/elements.h` | Probability that two different gases swap during a random walk (÷4 against stratification) |
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
| `BOIL_EXCESS_CAP_K` | 20 K | `sim/heat.c` | Freshly boiled steam starts at most this far above 100 °C (no gas mass until M4) |
| `EDIT_HEAT_STEP_K` | 8 K | `sim/edit.h` | Heat / Cool brush change per application (60 per second while held) |

## Gameplay deviations from reality

Keep this list complete.

1. Gases are tinted so they are visible.
2. Heat conduction is sped up: 1000× in solids and liquids, 10× where a gas is involved
   (`HEAT_TIME_SCALE`, `HEAT_TIME_SCALE_GAS`). Relative conductivities stay real.
3. M1–M3 gas is one particle per pixel at fixed density, with no pressure, until M4. So boiling
   turns one water cell into one steam cell (mass not conserved), and freshly boiled steam's
   temperature is capped (`BOIL_EXCESS_CAP_K`).
4. Liquids are incompressible. Pressure moves water from the top of a body straight to the
   opening it is pushed toward, and the flow rate through an opening is not proportional to the
   square root of the depth (see SIMULATION.md → Liquid pressure).
5. No heat radiation: vacuum is a perfect insulator (D-017).
6. Until M4 there is no nitrogen, so "air" is pure oxygen.

## Adding an element (checklist)

1. Add a row to the table above, with sourced reference values and the milestone.
2. Add the entry to the definition table in `src/sim/elements.c`. Keep IDs stable, because saves
   (M6) will depend on them.
3. If it is paintable, add it to the toolbar in `src/game/`.
4. Add behaviour tests for anything new (a new behaviour, a phase change, a reaction).
5. Update "Gameplay deviations" if anything is non-physical.
