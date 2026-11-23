#pragma once

// Creatures (M4c): entities with their own position, body and physics, stamped into the grid as
// ELEM_BODY cells. docs/SIMULATION.md -> Creatures; docs/DECISIONS.md D-027.
//
// Creatures are updated serially at the start of each tick, in list order, so results do not
// depend on the number of threads. Their body cells are solid to every rule of the grid
// (BEHAVIOR_STATIC): only the creature moves them.

#include "sim/world.h"

#define CREATURE_MAX    64
#define CREATURE_W      9 // body size in cells: 45 cm x 1.75 m at 5 cm per cell
#define CREATURE_H      35
#define CREATURE_MOUTH  3       // mouth row from the top of the body
#define CREATURE_BODY_K 310.15f // body temperature, 37 degC

typedef enum CreatureState {
    CREATURE_OK = 0,
    CREATURE_IMPAIRED = 1, // little oxygen: slower
    CREATURE_DANGER = 2,   // too little oxygen or too much CO2: losing health
    CREATURE_DEAD = 3,
} CreatureState;

typedef struct Creature {
    int32_t x, y;     // top-left cell of the body
    float vx, vy;     // velocity, cells per tick (+y is down)
    float fx, fy;     // movement accumulated but not yet done (less than one cell)
    int8_t facing;    // +1 right, -1 left (where the mouth is)
    int8_t walk;      // wanted walking direction: -1, 0, +1
    uint8_t wander;   // pick `walk` by itself (the test creature)
    uint8_t grounded; // standing on something solid
    uint8_t state;    // CreatureState
    uint8_t pad[3];
    uint32_t id;            // spawn order; seeds its randomness
    float health;           // 0..1; 0 is dead
    float metabolism_scale; // breathing and health run this much faster than real time (1)
    int32_t breath_timer;   // ticks to the next breath
    float po2, pco2;        // partial pressures of O2 and CO2 around the head, Pa
    float o2_short;         // fraction of the last breath's oxygen it could not get
    float submerged;        // fraction of its sides in liquid
    float heat_w;           // body heat given off in the last tick, W (game time)
    double o2_used;         // kg of O2 taken from the air, in total
    double co2_made;        // kg of CO2 breathed out, in total
} Creature;

// Puts a creature with its top-left cell at (x, y). The area must hold no solid or liquid; gas in
// it is squeezed into the gas around. Returns its index, or -1.
int creature_spawn(World *w, int x, int y);

// Moves, breathes and heats every creature. sim_tick calls it first.
void creatures_update(World *w);
