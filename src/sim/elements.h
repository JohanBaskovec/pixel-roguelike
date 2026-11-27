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
    // Gas species (M6a, D-035): a gas cell is ELEM_GAS and holds a mass of each. These IDs name
    // the species (their rows give their properties); no cell holds them. Painting one paints a
    // gas cell of that species alone.
    ELEM_OXYGEN = 6,
    ELEM_CO2 = 7,
    ELEM_HYDROGEN = 8,
    ELEM_STEAM = 9, // water vapour
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
    ELEM_LEAD = 20,   // solid lead (molten lead, or bullets melted together, solidified)
    ELEM_MOLTEN_LEAD = 21,
    ELEM_BRASS = 22, // solid brass (molten casings, solidified)
    ELEM_MOLTEN_BRASS = 23,
    ELEM_GAS = 24, // a gas cell: a mixture of the gas species (GasMix)
    ELEM_BLOOD = 25, // blood (M6b): a liquid a little denser than water
    ELEM_WOOD = 26,         // wood (M6b, D-044): solid, burns
    ELEM_BURNING_WOOD = 27, // wood on fire: burns while oxygen reaches it; `aux` is how much is gone
    ELEM_SAWDUST = 28,      // wood crumbs (D-045): a powder, what slivers cut from a log become
    ELEM_WALKER_CORPSE = 29, // flesh of a dead walker (D-046): static, part of a rigid body
    ELEM_FLYER_CORPSE = 30,  // flesh of a dead flyer
    // The gases as liquids and solids (D-057).
    ELEM_LIQUID_OXYGEN = 31,
    ELEM_SOLID_OXYGEN = 32,
    ELEM_LIQUID_NITROGEN = 33,
    ELEM_SOLID_NITROGEN = 34,
    ELEM_LIQUID_HYDROGEN = 35,
    ELEM_SOLID_HYDROGEN = 36,
    ELEM_DRY_ICE = 37,     // solid CO2: a powder (no liquid CO2 at 1 atm)
    ELEM_ROCK_VAPOUR = 38, // a gas species: boiled lava
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
// Film condensation (D-056): water vapour gives its heat to matter colder than its dew point far
// faster than still gas conducts. The heat transfer coefficient of pure steam on a cold surface,
// W/(m^2 K), in real time (no speed-up); in a mixture, times the vapour's mole fraction in excess
// of saturation at the surface.
#define CONDENSE_FILM_H 8000.0f

// Radiation (D-052): matter hotter than RAD_MIN_K sends heat out through the air, in rays that are
// absorbed by the first matter they meet within RAD_RANGE cells (air is transparent; a ray that
// finds nothing leaves for good). Power per exposed face: emissivity * sigma * (T^4 - T_room^4) *
// (5 cm)^2, sped up by RAD_TIME_SCALE (a deviation: reality's would be HEAT_TIME_SCALE, and a thin
// film of lava would freeze within a second; at 30 it crusts over in seconds).
#define RAD_MIN_K       600.0f
#define RAD_EMISSIVITY  0.9f
#define RAD_SIGMA       5.670374e-8f // W/(m^2 K^4)
#define RAD_TIME_SCALE  30.0f
#define RAD_RANGE       32

// Gases (M4): each gas cell holds a mass; pressure follows the ideal gas law.
#define GAS_R   8.314462f // J/(mol*K)
#define P_ATM   101325.0f // Pa
#define T_ROOM  293.15f   // K
// Heat and cold hurt (D-050). A body is hurt by the temperature of its skin (the mean of its
// outer cells) and of what touches it (air, water, ground): above 45 degC either burns, and what
// touches it below the freezing point freezes; so does the skin below its cold limit; the further
// beyond, the more damage per second. The skin's cold limit is 15 degC for a person (whose body heat holds the skin
// up to 37 degC while it can: a skin that cold means the heat it loses is more than it makes) and
// the freezing point for a monster (it has no body heat: it is as warm as the room).
#define T_HOT_DANGER         318.15f // K (45 degC)
#define T_COLD_DANGER_PERSON 288.15f // K (15 degC)
#define T_COLD_DANGER_BEAST  273.15f // K (0 degC)
#define THERMAL_DPS_PER_K    0.5f    // hit points a second for each kelvin beyond the limit
#define THERMAL_DPS_MAX      60.0f
#define GRAVITY 9.81f     // m/s^2

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

// Burning wood (M6b, D-044; sim.c update_burning_wood). Wood heated to WOOD_IGNITE_K catches fire
// and goes out below WOOD_OUT_K. A burning cell takes oxygen from the gas cells beside it that
// hold at least WOOD_MIN_O2 of it by volume (a fire dies in air below 16% oxygen), at most
// WOOD_BURN_TICKS ticks for a whole cell, and gives off carbon dioxide and water vapour into them
// (cellulose, C6H10O5 + 6 O2 -> 6 CO2 + 5 H2O: per kg of wood 1.185 kg O2, 0.556 kg H2O, the
// rest CO2) and WOOD_LHV J per kg of wood, which heats the cell (up to WOOD_FLAME_K).
#define WOOD_IGNITE_K   523.15f // 250 degC (piloted ignition of wood: 250-300 degC)
#define WOOD_OUT_K      480.0f
#define WOOD_FLAME_K    1400.0f
#define WOOD_BURN_TICKS 240 // 10 s for a cell at full burn
#define WOOD_FUEL_SCALE 0.02f // see below
#define WOOD_HEAT_SCALE 10.0f
#define WOOD_GAS_COUPLING 100.0f // hot gas heats wood this many times faster than air conducts
#define WOOD_SPREAD     4.0f // flame contact heats unburnt wood this many times faster than conduction
#define WOOD_FUEL_HEAT  0.7f // share of the heat of combustion that goes back into the wood
#define WOOD_MIN_O2     0.16f
#define WOOD_LHV        16.0e6f
#define WOOD_O2_PER_KG  1.18519f // 6 O2 per C6H10O5
#define WOOD_H2O_PER_KG 0.55602f // 5 H2O
#define WOOD_CO2_PER_KG (1.0f + WOOD_O2_PER_KG - WOOD_H2O_PER_KG) // mass is conserved exactly

// Gas species (D-035): a gas cell holds a mass of each, in this order.
#define GAS_SPECIES 6
enum { GAS_O2 = 0, GAS_CO2 = 1, GAS_H2 = 2, GAS_H2O = 3, GAS_N2 = 4, GAS_ROCK = 5 };
extern const ElementId g_species_elem[GAS_SPECIES]; // the element naming each species
// Species index + 1 of a species element (ELEM_OXYGEN..ELEM_NITROGEN); 0 for any other element.
extern uint8_t g_gas_slot[256];
static inline int elem_species(ElementId e) {
    return (int)g_gas_slot[e] - 1;
}
// Species tables (generated).
extern float g_species_molar[GAS_SPECIES];     // kg/mol
extern float g_species_inv_molar[GAS_SPECIES]; // mol/kg
extern float g_species_cp[GAS_SPECIES];        // J/(kg K)
extern float g_species_k[GAS_SPECIES];         // W/(m K)
// What each species condenses to (dry ice for CO2: it has no liquid at 1 atm), the temperature it
// does so at 1 atm (K) and the heat it gives off (J/kg).
extern const ElementId g_species_condensed[GAS_SPECIES];
extern float g_species_boil[GAS_SPECIES];
extern float g_species_latent[GAS_SPECIES];
// The species an element boils (or sublimes) into (elements whose g_elem_above is ELEM_GAS).
extern uint8_t g_boil_species[ELEM_COUNT];
// Below this every gas cell may hold something that condenses; above it only water vapour and
// rock vapour do.
#define GAS_CRYO_K 195.0f
// Air as the Air brush paints it, by mole fraction: 78.1% N2 + 0.9% argon (counted as N2), 20.95%
// O2, 0.04% CO2; dry.
#define AIR_O2_FRAC  0.2095f
#define AIR_CO2_FRAC 0.0004f

// Buoyancy scale K_BUOY (docs/SIMULATION.md), x/65536.
#define K_BUOY_16 65536u

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
extern float g_elem_molar_mass[256]; // kg/mol (species elements)
extern float g_gas_mass_atm[256];    // mass of one cell of the species at 1 atm and 20 degC, kg
// Moles in one cell at 1 atm and 20 degC (any gas). A gas cell splits into vacuum only if it
// holds at least GAS_SPLIT_MOLES (twice the smallest cell a split may leave, GAS_MIN_FRAC of
// that); thinner gas moves as a single particle.
#define GAS_MOLES_ATM   (P_ATM * CELL_VOLUME_M3 / (GAS_R * T_ROOM))
#define GAS_MIN_FRAC    0.01f
#define GAS_SPLIT_MOLES (2.0f * GAS_MIN_FRAC * GAS_MOLES_ATM)

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

// Density of a fluid at physical temperature t (K), for buoyancy and convection. A gas (ELEM_GAS
// counts as air; sim.c uses a cell's own mixture) follows the ideal gas law at 1 atm whatever its
// pressure (a parcel at another pressure would be squeezed or expanded to its surroundings'
// pressure long before it rose or sank); water uses its real density curve (densest at 4 °C).
float elem_fluid_density(ElementId e, float t);

// Pressure (Pa) of n moles of gas in one cell at temperature t (K).
static inline float gas_pressure_moles(float n, float t) {
    return n * GAS_R * t * (1.0f / CELL_VOLUME_M3);
}

// Height in cells of a column of liquid `e` that weighs as much as pressure p (Pa), rounded:
// gas pressure acting on a liquid, in the units water pressure works in. 1 atm on water is 207.
static inline int32_t liquid_head(ElementId e, float p) {
    float h = p / (g_elem_defs[e].density * GRAVITY * CELL_SIZE_M);
    return (int32_t)PR_MIN(h + 0.5f, 1e6f);
}

// Mass (kg) of one cell of gas species element e at pressure p (Pa) and temperature t (K).
static inline float gas_mass_at(ElementId e, float p, float t) {
    return p * CELL_VOLUME_M3 * g_elem_molar_mass[e] / (GAS_R * t);
}

// The highest pressure (Pa) a cell of gas species element e (or ELEM_STEAM...) can be given at
// temperature t (K) before it would become a liquid: steam, at its saturation pressure (above it,
// it condenses: sim.c); the others, at the density of their liquid (no gas can be denser than its
// liquid; hydrogen, nitrogen and oxygen cannot be liquefied by pressure at room temperature, so
// this is a limit of the painting tool, not of the simulation, which has no cap).
float gas_max_pressure(ElementId e, float t);

// Saturation pressure (Pa) of species s at temperature t (K), and the temperature at which a partial
// pressure p (Pa) of it saturates (dew point): water by Antoine's equation, the others by
// Clausius-Clapeyron from the boiling point (above liquid and solid alike, an approximation).
float gas_saturation_pressure(int s, float t);
float gas_dew_point(int s, float p);

// Can this mixture hold vapour that condenses at temperature t? (Cheap: called per gas cell.)
static inline bool gas_may_condense(const float *m, float t) {
    return m[GAS_H2O] > 0.0f || m[GAS_ROCK] > 0.0f || t < GAS_CRYO_K;
}

// The form element e takes at temperature t (K) at 1 atm: water is ice, water or water vapour; rock
// is rock, lava or rock vapour; oxygen is solid, liquid or gas. Returns the species element for a
// gas.
ElementId elem_form_at(ElementId e, float t);

// The most gas species element e a cell may be painted with (kg): the mass of its liquid.
float gas_max_mass(ElementId e);

// Saturation pressure of water vapour (Pa) at temperature t (K): Antoine's equation, two ranges
// (1-100 degC, 99-374 degC); at or below 0 degC it is extrapolated (over ice it would be a little
// lower).
float water_saturation_pressure(float t);
