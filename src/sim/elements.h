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

// Buoyancy scale K_BUOY and gas mixing probability P_GAS_MIX (docs/SIMULATION.md), x/65536.
#define K_BUOY_16    65536u
#define P_GAS_MIX_16 4096u

extern const ElementDef g_elem_defs[ELEM_COUNT];

// Lookup tables generated from g_elem_defs by elements_init(). Indexed by any ElementId,
// including ELEM_OUT, so a cell's element can be used without a bounds check.
extern uint8_t g_elem_behavior[256];
extern uint32_t g_elem_density_q[256]; // density in g/m^3, for integer comparisons
extern uint8_t g_elem_spread[256];
// Probability (x/65536) that `self` swaps upward with a denser fluid `above`:
// K_BUOY * (rho_above - rho_self) / (rho_above + rho_self).
extern uint16_t g_buoy_p[ELEM_COUNT][ELEM_COUNT];

// Builds the lookup tables. Idempotent; must run before any simulation (world_create calls it).
void elements_init(void);

static inline bool elem_is_fluid(ElementId e) {
    return g_elem_behavior[e] >= BEHAVIOR_LIQUID;
}

static inline const char *elem_name(ElementId e) {
    return e < ELEM_COUNT ? g_elem_defs[e].name : "Out of world";
}
