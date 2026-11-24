#pragma once

// Weapons (M6a): the player's automatic rifle. docs/SIMULATION.md -> Rifle and bullets.
//
// A shot puts a bullet (ELEM_BULLET, a free particle) in flight from the shooter's shoulder toward
// the aim point, and ejects the spent casing (ELEM_CASING, a particle) from the rifle. Bullets dig
// through dirt and rock, throw the debris out, stir the air they pass through, and fall to the
// ground when they stop; casings and spent bullets pile up as pixels (particle.c). Called serially
// between ticks, so results do not depend on the number of threads.

#include "sim/creature.h"

// Muzzle velocity: 900 m/s (an assault rifle), in cells per tick.
#define RIFLE_BULLET_SPEED (900.0f / CELL_SIZE_M * TICK_SECONDS)
#define RIFLE_INTERVAL     5      // ticks between shots: 720 rounds per minute
#define RIFLE_SPREAD       0.005f // radians, either way (about 0.3 degrees)
#define RIFLE_SHOULDER     4      // rows from the top of the body: the shoulder, 65 cm up
#define RIFLE_LENGTH       9      // cells (45 cm, at the body's half size)
#define RIFLE_PORT         4      // ejection port, cells along the rifle from the shoulder

typedef struct Rifle {
    int32_t cooldown; // ticks until it can fire again
    uint32_t shots;   // fired so far; seeds each shot's randomness
} Rifle;

// Where the rifle's stock rests: the shooter's shoulder, in cells.
void rifle_shoulder(const Creature *c, float *x, float *y);

// One tick of the rifle held by creature `shooter`, aimed at (aim_x, aim_y) in cells: fires if
// the trigger is held and it is ready (and the shooter alive). Returns true if it fired.
bool rifle_update(World *w, Rifle *r, int shooter, float aim_x, float aim_y, bool trigger);
