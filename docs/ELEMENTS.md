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
| 6 | Oxygen (O₂) | GAS | 1.429 | 918 | 0.026 | condenses 90.2 K (liquid O₂, later) | `#9CC8F0` α 0.18 | 1 |
| 7 | Carbon dioxide (CO₂) | GAS | 1.977 | 844 | 0.0166 | deposits 194.7 K → dry ice (later) | `#5A5A5A` α 0.30 | 1 |
| 8 | Hydrogen (H₂) | GAS | 0.0899 | 14300 | 0.18 | condenses 20.3 K (later) | `#E9A6E0` α 0.15 | 1 |
| 9 | Steam | GAS | 0.59 (at 100 °C) | 2080 | 0.025 | condenses 373.15 K → water | `#E6ECF2` α 0.25 | 3 |

Notes
- **Dirt** is a bulk soil value. Real soil ranges from 1200 to 1700 kg/m³, and k from 0.25 (dry) to
  2 (wet).
- **Gas colours** are a deliberate deviation, since real gases are invisible. The α is the blend
  over the background at full concentration. From M4 it scales with mass.
- **Molar masses** (needed from M4): O₂ 0.032, CO₂ 0.044, H₂ 0.002, H₂O 0.018 kg/mol.
- **Default spawn temperature** for all painted elements is 293.15 K (20 °C). Exceptions: ice
  spawns at 263.15 K and steam at 383.15 K, so they do not change phase immediately once heat
  exists.

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

Per-element movement constants. These are game-feel values, not physics.

| Element | Dispersion (liquids, cells/tick) | Notes |
|---|---|---|
| Water | 5 | Clamped to `MAX_MOVE` = 32 |

| Global | Value | Meaning |
|---|---|---|
| `K_BUOY` | TBD in M1 | Scales the buoyancy swap probability between fluids |
| `P_GAS_MIX` | TBD in M1 | Base probability that two different gases swap during a random walk |
| `HEAT_TIME_SCALE` | TBD in M3 | Global speed-up of conduction (see SIMULATION.md) |
| `HEAT_EPSILON_K` | TBD in M3 | Temperature difference below which a chunk may sleep thermally |

## Gameplay deviations from reality

Keep this list complete.

1. Gases are tinted so they are visible.
2. Heat conduction is uniformly sped up by `HEAT_TIME_SCALE` (M3).
3. M1–M3 gas is one particle per pixel at fixed density, with no pressure, until M4.
4. Liquids are incompressible and have no hydrostatic pressure (communicating vessels do not
   equalise).
5. Vacuum does not radiate heat.

## Adding an element (checklist)

1. Add a row to the table above, with sourced reference values and the milestone.
2. Add the entry to the definition table in `src/sim/elements.c`. Keep IDs stable, because saves
   (M6) will depend on them.
3. If it is paintable, add it to the toolbar in `src/game/`.
4. Add behaviour tests for anything new (a new behaviour, a phase change, a reaction).
5. Update "Gameplay deviations" if anything is non-physical.
