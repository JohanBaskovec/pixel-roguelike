#pragma once

// Creatures (M4c): entities with their own position, body and physics, stamped into the grid as
// ELEM_BODY cells. docs/SIMULATION.md -> Creatures; docs/DECISIONS.md D-027.
//
// Creatures are updated serially at the start of each tick, in list order, so results do not
// depend on the number of threads. Their body cells are solid to every rule of the grid
// (BEHAVIOR_STATIC): only the creature moves them.

#include "sim/particle.h"
#include "sim/world.h"

#define CREATURE_MAX   128
#define CREATURE_MAX_W 16
#define CREATURE_MAX_H 48
// Bodies are drawn at half a person's size (25 cm x 85 cm at 5 cm per cell), for the view to show
// more of the world around them; the world's physics keeps its scale. Their breathing and body
// heat stay a person's, and they run and jump as fast and as high (docs/ELEMENTS.md ->
// Deviations).
#define CREATURE_W      5 // default body size in cells: 25 cm x 85 cm at 5 cm per cell
#define CREATURE_H      17
#define CREATURE_MOUTH  2       // mouth row from the top of the body
#define CREATURE_BODY_K 310.15f // body temperature, 37 degC
// The player (M6a): 85 cm tall, 25 cm wide; runs at 4 m/s.
#define PLAYER_W     5
#define PLAYER_H     17
#define PLAYER_SPEED (4.0f / CELL_SIZE_M * TICK_SECONDS)
// Walking at 1.4 m/s (a creature's default `speed`), cells per tick.
#define CREATURE_WALK_SPEED (1.4f / CELL_SIZE_M * TICK_SECONDS)
// A jump takes off at 3.1 m/s: about 50 cm high (v^2 / 2g), cells per tick.
#define CREATURE_JUMP_SPEED (3.13f / CELL_SIZE_M * TICK_SECONDS)

// The jetpack (M6b; infinite fuel for now): while the jump key stays held for JETPACK_DELAY ticks
// (a tap is still a plain jump) and the body is in the air, it pushes up with JETPACK_ACCEL, a game
// invention, until it rises at JETPACK_MAX_RISE. Net of gravity it lifts at 1.2 g.
#define JETPACK_DELAY    12
#define JETPACK_ACCEL    (2.2f * PARTICLE_GRAVITY)
#define JETPACK_MAX_RISE (5.0f / CELL_SIZE_M * TICK_SECONDS)

// What a creature is (M6b). Monsters do not breathe and give off no body heat (they are boxes of
// body cells that chase the player: enemy.h); the player is not hurt by its own bullets.
typedef enum CreatureKind {
    KIND_PERSON = 0, // the test creature: breathes, wanders
    KIND_PLAYER = 1,
    KIND_WALKER = 2, // a small monster that walks
    KIND_FLYER = 3,  // a monster that flies
} CreatureKind;

typedef enum CreatureState {
    CREATURE_OK = 0,
    CREATURE_IMPAIRED = 1, // little oxygen: slower
    CREATURE_DANGER = 2,   // too little oxygen or too much CO2: losing health
    CREATURE_DEAD = 3,
} CreatureState;

typedef struct Creature {
    int32_t x, y;           // top-left cell of the body
    int16_t w, h;           // body size in cells
    float vx, vy;           // velocity, cells per tick (+y is down)
    float fx, fy;           // movement accumulated but not yet done (less than one cell)
    int8_t facing;          // +1 right, -1 left (where the mouth is)
    int8_t walk;            // wanted walking direction: -1, 0, +1
    uint8_t wander;         // pick `walk` by itself (the test creature)
    uint8_t grounded;       // standing on something solid
    uint8_t state;          // CreatureState
    uint8_t jump;           // wants to jump (done when it stands on something; then cleared)
    uint8_t thrust;         // the jump key is held (the jetpack fires after JETPACK_DELAY ticks)
    uint8_t jet_hold;       // ticks `thrust` has been held without a break (saturates)
    uint8_t kind;           // CreatureKind
    uint8_t alert;          // monsters: has seen the player and chases it until it is off the screen
    uint8_t hit_flash;      // ticks left of the flash after a hit (drawing)
    uint8_t attack_cd;      // monsters: ticks until the next bite
    float hp, hp_max;       // hit points (damage from bullets and bites; 0 is dead)
    float bleed;            // droplets of blood a dead body still has to bleed
    float px_debt;          // monsters: pixels owed for the hit points lost (enemy.c)
    uint8_t px_lost;        // monsters: pixels lost so far
    uint8_t corpse;         // a dead monster, now a rigid body (rigid.h): this entry is inert
    uint8_t aimed;          // `facing` is set from outside (the player aims), not from `walk`
    uint8_t holding;        // holds a breath in (it could not breathe out: the mouth is in liquid)
    float speed;            // walking speed, cells per tick
    uint32_t id;            // spawn order; seeds its randomness
    float health;           // 0..1; 0 is dead
    float metabolism_scale; // breathing and health run this much faster than real time (1)
    int32_t breath_timer;   // ticks to the next breath in
    GasMix lungs;           // gas breathed in and not yet out
    float po2, pco2;        // partial pressures of O2 and CO2 around the head, Pa
    float o2_short;         // fraction of the last breath's oxygen it could not get
    float submerged;        // fraction of its sides in liquid
    float heat_w;           // body heat given off in the last tick, W (game time)
    double o2_used;         // kg of O2 the body has used, in total
    double co2_made;        // kg of CO2 the body has made, in total (in the lungs or breathed out)
} Creature;

// Puts a creature with its top-left cell at (x, y). The area must hold no solid or liquid; gas in
// it is squeezed into the gas around. Returns its index, or -1.
int creature_spawn(World *w, int x, int y);

// The same, with a body of w x h cells (at most CREATURE_MAX_W x CREATURE_MAX_H).
int creature_spawn_sized(World *w, int x, int y, int cw, int ch);

// Hit points of a person and of the player.
#define PERSON_HP 100.0f

// Moves, breathes and heats every creature. sim_tick calls it first.
void creatures_update(World *w);
