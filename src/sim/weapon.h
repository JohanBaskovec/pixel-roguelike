#pragma once

// Weapons (M6a): the player's automatic rifle. docs/SIMULATION.md -> Rifle and bullets.
//
// A shot puts a bullet (ELEM_BULLET, a free particle) in flight from the shooter's shoulder toward
// the aim point, and ejects the spent casing (ELEM_CASING, a particle) from the rifle. Bullets dig
// through dirt and rock, throw the debris out, stir the air they pass through, and fall to the
// ground when they stop; casings and spent bullets pile up as pixels (particle.c). Called serially
// between ticks, so results do not depend on the number of threads.

#include "sim/bullet.h"
#include "sim/creature.h"

// Muzzle velocity: 900 m/s (an assault rifle), in cells per tick.
#define RIFLE_BULLET_SPEED (900.0f / CELL_SIZE_M * TICK_SECONDS)
#define RIFLE_INTERVAL     5      // ticks between shots: 720 rounds per minute
#define RIFLE_SPREAD       0.005f // radians, either way (about 0.3 degrees)
#define RIFLE_SHOULDER     4      // rows from the top of the body: the shoulder, 65 cm up
#define RIFLE_LENGTH       9      // cells (45 cm, at the body's half size)
#define RIFLE_PORT         4      // ejection port, cells along the rifle from the shoulder

// Hit points a plain bullet takes from a creature.
#define RIFLE_DAMAGE 12.0f

// The gun (M6b, D-047): 1 to GUN_SLOTS_MAX slots, each the design of a bullet (bullet.h). It
// fires the slots one after the other, one shot each, FireInterval ticks apart; after the last
// it reloads for ReloadTicks, then starts again from the first. A zero Rifle (the tests') is the
// plain rifle: one plain bullet a shot, every RIFLE_INTERVAL ticks, no reloading.
#define GUN_SLOTS_MAX 10

typedef struct Rifle {
    int32_t cooldown; // ticks until it can fire again
    uint32_t shots;   // fired so far; seeds each shot's randomness
    uint8_t nslots;   // 0: the plain rifle; else 1..GUN_SLOTS_MAX
    int32_t fire_interval; // ticks between two shots (the slots)
    int32_t reload_ticks;  // ticks to reload after the last slot
    int32_t next_slot;     // the slot to fire next
    int32_t reload;        // ticks of reloading left (0: not reloading)
    BulletDesign slots[GUN_SLOTS_MAX];
} Rifle;

// The starting gun: 3 slots, each a single pixel of iron.
void rifle_init(Rifle *r);

#define GUN_START_INTERVAL 5  // ticks
#define GUN_START_RELOAD   60 // ticks

// Reloads now, before the last slot has been fired (the unfired slots are not lost). False if
// there is nothing to do: the gun is full, or reloading already.
bool rifle_reload(Rifle *r);

// Where the rifle's stock rests: the shooter's shoulder, in cells.
void rifle_shoulder(const Creature *c, float *x, float *y);

// One tick of the rifle held by creature `shooter`, aimed at (aim_x, aim_y) in cells: fires if
// the trigger is held and it is ready (and the shooter alive). Returns true if it fired.
bool rifle_update(World *w, Rifle *r, int shooter, float aim_x, float aim_y, bool trigger);
