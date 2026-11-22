#pragma once

#include "sim/world.h"

#define SCENE_WALL_THICKNESS 16

// Clears the world to void and builds dirt walls `thickness` cells thick around the edge.
void scene_empty_box(World *w, int thickness);

// The empty box plus some rock and iron, a pool of water and pockets of each gas.
void scene_demo(World *w);

// The benchmark scenario: the box filled with a random churning mix of water and gases over a
// rock floor. Deterministic for a given world seed.
void scene_bench(World *w);
