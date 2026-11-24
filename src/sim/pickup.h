#pragma once

// Crates (M6b): health, boxes lying on the ground (not cells). A crate falls if what it rests on
// goes; the player picks one up by touching it. (There were ammunition crates too, until the gun
// of bullet slots, D-047, which reloads by itself.)

#include "sim/creature.h"
#include "sim/world.h"

#define PICKUP_W      9 // cells
#define PICKUP_H      7
#define PICKUP_HEALTH_HP   40.0f

// Puts a crate of `kind` with its bottom row at row `bottom` and centred on column `cx`. Returns
// its index, or -1 (the list is full).
int pickup_add(World *w, int kind, int cx, int bottom);

// Falls, a cell a tick, while the cells under it are open. Frozen outside the simulated area.
// sim_tick calls it.
void pickups_update(World *w);

// The creature `who` (the player) touches crates: a health crate gives PICKUP_HEALTH_HP hit points
// (if it is hurt); a crate that is not needed stays. Returns the number picked up.
int pickups_collect(World *w, int who);
