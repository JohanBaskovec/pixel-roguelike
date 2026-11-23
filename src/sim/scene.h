#pragma once

#include "sim/world.h"

#define SCENE_WALL_THICKNESS 16

// Clears the world to void and builds dirt walls `thickness` cells thick around the edge.
void scene_empty_box(World *w, int thickness);

// The default level (M6a): the box with a hilly dirt ground (lower 45%) holding rock strata and
// boulders, and air (1 atm) above it.
void scene_level(World *w);

// Puts the player (PLAYER_W x PLAYER_H) standing on whatever is below column x (the first cell
// from the top that is not void or gas). Returns its creature index, or -1.
int scene_spawn_player(World *w, int x);

// The empty box plus some rock and iron, a pool of water and pockets of each gas.
void scene_demo(World *w);

// The benchmark scenario: a rock floor (bottom quarter), and above it a band of random churning
// mix of water, gases and vacuum filling `fill` (0..1) of the space up to the ceiling; the rest is
// vacuum. Deterministic for a given world seed.
void scene_bench(World *w, float fill);
