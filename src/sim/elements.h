#pragma once

#include "core/common.h"

// Element IDs are stable: saves (M6) will depend on them. Keep in sync with docs/ELEMENTS.md.
typedef uint8_t ElementId;

enum {
    ELEM_VOID = 0,
    ELEM_DIRT = 1,
    ELEM_ROCK = 2,
    ELEM_IRON = 3,
    ELEM_ICE = 4,
    ELEM_WATER = 5,
    ELEM_OXYGEN = 6,
    ELEM_CO2 = 7,
    ELEM_HYDROGEN = 8,
    ELEM_STEAM = 9,
    ELEM_NITROGEN = 10,
    ELEM_BODY = 11, // a creature's body (M4c): solid to matter, moved only by its creature
    ELEM_SAND = 12,
    ELEM_SOIL = 13,   // loose soil: dug dirt
    ELEM_RUBBLE = 14, // dug rock
    ELEM_LAVA = 15,   // molten rock
    ELEM_MOLTEN_IRON = 16,
    ELEM_OIL = 17,
    ELEM_BULLET = 18, // a spent bullet (lead): falls and piles like a powder
    ELEM_CASING = 19, // a spent cartridge case (brass)
    ELEM_COUNT,

    // Pseudo element returned for coordinates outside the world. Behaves as an immovable wall.
    ELEM_OUT = 255,
};

// Ordered so that "does not move by itself" is a single comparison: behavior <= BEHAVIOR_STATIC.
typedef enum Behavior {
    BEHAVIOR_NONE = 0,
    BEHAVIOR_STATIC = 1,
    BEHAVIOR_POWDER = 2,
    BEHAVIOR_LIQUID = 3,
    BEHAVIOR_GAS = 4,
} Behavior;

typedef struct ElementDef {
    const char *name;
    Behavior behavior;
    float density;     // kg/m^3
    float heat_cap;    // J/(kg*K)
    float conduct;     // W/(m*K)
    float spawn_temp;  // K, temperature of freshly painted cells
    uint32_t color;    // 0xRRGGBB base colour
    uint8_t alpha;     // opacity over the void background (gases, liquids)
    uint8_t shade_var; // per-pixel brightness variation amplitude
    uint8_t spread;    // liquids: sideways dispersion; gases: random-walk step (cells/tick)
} ElementDef;

#define MAX_MOVE_CELLS 32 // CHUNK_SIZE / 2; the threading invariant (docs/ARCHITECTURE.md)

// Physical scale and heat (docs/SIMULATION.md -> Units and scale, Heat pass).
#define CELL_SIZE_M    0.05f // one pixel is a 5 cm cube
#define CELL_VOLUME_M3 (CELL_SIZE_M * CELL_SIZE_M * CELL_SIZE_M)
#define TICK_SECONDS   (1.0f / 60.0f)
// Conduction runs this much faster than reality (a deviation, docs/ELEMENTS.md), so heat moves
// through solids at a playable pace. Gases get a much smaller speed-up: in air, convection (which
// runs in real time) must stay faster than conduction, or warm air diffuses away instead of
// rising. Between two materials the smaller factor applies.
#define HEAT_TIME_SCALE     1000.0f
#define HEAT_TIME_SCALE_GAS 10.0f

// Gases (M4): each gas cell holds a mass; pressure follows the ideal gas law.
#define GAS_R       8.314462f // J/(mol*K)
#define P_ATM       101325.0f // Pa
#define T_ROOM      293.15f   // K
#define AIR_O2_FRAC 0.21f     // the Air tool: 21% oxygen, the rest nitrogen (argon counted as N2)
#define GRAVITY     9.81f     // m/s^2

// A phase change: when a cell passes `temp` (K), it becomes `to`, absorbing or releasing
// `latent` J/kg. temp == 0: none.
typedef struct PhaseChange {
    float temp;
    ElementId to;
    float latent;
} PhaseChange;

extern const PhaseChange g_elem_below[ELEM_COUNT]; // cooled below: freezing, condensing
extern const PhaseChange g_elem_above[ELEM_COUNT]; // heated above: melting, boiling

// Reactions (M5): a cell of `a` next to a cell of `b`, hot enough, reacts into `product`, using
// b_per_a kg of b per kg of a and releasing heat_per_kg J per kg of a. docs/SIMULATION.md ->
// Reactions. Indexed by g_reaction_of[a] - 1 (0: none).
typedef struct Reaction {
    ElementId a, b, product;
    float ignite_k;    // reacts if `a` or one of its neighbours is at least this hot (K)
    float b_per_a;     // kg of b used per kg of a
    float heat_per_kg; // J released per kg of a
} Reaction;
#define REACTION_COUNT 1
#define FLAME_MAX_K    3000.0f // products are not heated beyond this (dissociation, not modelled)
extern const Reaction g_reactions[REACTION_COUNT];
extern uint8_t g_reaction_of[256];
extern float g_min_ignite_k; // lowest ignition temperature of any reaction

// Buoyancy scale K_BUOY and gas mixing probabilities (docs/SIMULATION.md), x/65536: P_GAS_MIX in
// still gas (diffusion), P_GAS_MIX_STIRRED where the gas has a current (turbulence).
#define K_BUOY_16            65536u
#define P_GAS_MIX_16         1024u
#define P_GAS_MIX_STIRRED_16 32768u

extern const ElementDef g_elem_defs[ELEM_COUNT];

// Lookup tables generated from g_elem_defs by elements_init(). Indexed by any ElementId,
// including ELEM_OUT, so a cell's element can be used without a bounds check.
extern uint8_t g_elem_behavior[256];
extern uint32_t g_elem_density_q[256]; // density in g/m^3, for integer comparisons
extern uint8_t g_elem_spread[256];
// Probability (x/65536) that `self` swaps upward with a denser fluid `above`:
// K_BUOY * (rho_above - rho_self) / (rho_above + rho_self).
extern uint16_t g_buoy_p[ELEM_COUNT][ELEM_COUNT];

// Gas tables (generated). Indexed by any ElementId; 0 for non-gases.
extern float g_elem_molar_mass[256]; // kg/mol
extern float g_gas_inv_molar[256];   // 1 / molar mass
extern float g_gas_mass_atm[256];    // mass of one cell at 1 atm and 20 degC, kg
// A gas cell splits into vacuum only if it holds at least this mass (twice the smallest cell
// a split may leave: GAS_MIN_FRAC of the 1-atm mass). Thinner gas moves as single particles.
extern float g_gas_split_mass[256];
extern float g_gas_buoy_density[256]; // at 0 degC (O2 and N2: air); 0 if not a gas
#define GAS_MIN_FRAC 0.01f
// Buoyancy treats oxygen and nitrogen as air: same density at the same temperature
// (docs/SIMULATION.md -> Gases). Otherwise air, painted as separate N2 and O2 cells, would unmix
// in seconds; real air takes kilometres of height to separate.
#define AIR_DENSITY_0C 1.2922f // kg/m^3 at 0 degC, 1 atm

// Heat tables (generated).
extern float g_elem_cap[256];     // thermal mass of one cell, J/K (0 for void)
extern float g_elem_inv_cap[256]; // 1 / g_elem_cap, 0 for void
extern float g_elem_mass[256];    // mass of one cell, kg (gases: nominal, at 1 atm 20 degC)
extern float g_elem_cp[256];      // specific heat capacity, J/(kg*K)
extern float g_elem_low_t[256];   // temperature of the "below" phase change, 0 if none
extern float g_elem_high_t[256];  // temperature of the "above" phase change, 0 if none
// Heat exchanged per tick between two neighbouring cells, per kelvin of difference (J/K):
// min(conductance * dt * HEAT_TIME_SCALE, stability limit). Symmetric, so energy is conserved.
// For gases the limit uses the nominal (1 atm) mass; the heat pass applies the actual one.
extern float g_heat_coef[ELEM_COUNT][ELEM_COUNT];
extern float g_heat_coef_raw[ELEM_COUNT][ELEM_COUNT]; // without the stability limit

// Builds the lookup tables. Idempotent; must run before any simulation (world_create calls it).
void elements_init(void);

static inline bool elem_is_fluid(ElementId e) {
    return g_elem_behavior[e] >= BEHAVIOR_LIQUID;
}

static inline const char *elem_name(ElementId e) {
    return e < ELEM_COUNT ? g_elem_defs[e].name : "Out of world";
}

// A cell's stored temperature also holds latent heat: water above 100 °C is boiling, the excess
// being heat absorbed toward becoming steam (docs/SIMULATION.md -> Phase changes). Its physical
// temperature, used for conduction, buoyancy and display, stops at the transition point.
static inline float elem_temp(ElementId e, float t) {
    float hi = g_elem_high_t[e], lo = g_elem_low_t[e];
    if (hi > 0.0f && t > hi) {
        return hi;
    }
    if (lo > 0.0f && t < lo) {
        return lo;
    }
    return t;
}

// Progress of a phase change in [0, 1) (melting/boiling if heating, freezing/condensing if
// cooling), or -1 if the cell is not changing phase.
float elem_phase_progress(ElementId e, float t, bool *heating);

// Density of a fluid at physical temperature t (K), for buoyancy and convection. Gases follow the
// ideal gas law at 1 atm whatever their pressure (a parcel at another pressure would be squeezed
// or expanded to its surroundings' pressure long before it rose or sank), with oxygen and
// nitrogen counted as air; water uses its real density curve (densest at 4 °C).
float elem_fluid_density(ElementId e, float t);

// Pressure (Pa) of a gas cell holding mass m (kg) at physical temperature t (K).
static inline float gas_pressure(ElementId e, float m, float t) {
    return m * g_gas_inv_molar[e] * GAS_R * t * (1.0f / CELL_VOLUME_M3);
}

// Height in cells of a column of liquid `e` that weighs as much as pressure p (Pa), rounded:
// gas pressure acting on a liquid, in the units water pressure works in. 1 atm on water is 207.
static inline int32_t liquid_head(ElementId e, float p) {
    float h = p / (g_elem_defs[e].density * GRAVITY * CELL_SIZE_M);
    return (int32_t)PR_MIN(h + 0.5f, 1e6f);
}

// Mass (kg) of a gas cell at pressure p (Pa) and temperature t (K).
static inline float gas_mass_at(ElementId e, float p, float t) {
    return p * CELL_VOLUME_M3 * g_elem_molar_mass[e] / (GAS_R * t);
}
