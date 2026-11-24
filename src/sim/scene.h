#pragma once

#include "sim/world.h"

#define SCENE_WALL_THICKNESS 16

// Clears the world to void and builds dirt walls `thickness` cells thick around the edge.
void scene_empty_box(World *w, int thickness);

// The default level (M6b): a maze of air (1 atm) carved in dirt with boulders of rock. Rooms are
// MAZE_ROOM_H (3 player heights) tall and about a screen wide (MAZE_ROOM_W; at zoom x2); they are
// the cells of a perfect maze, one room wide and MAZE_ROOM_H + MAZE_WALL cells tall each, and
// openings join them: doorways in the side walls, shafts (MAZE_ROOM_H wide) through the floors.
// Deterministic for the world's seed.
#define MAZE_ROOM_H 51 // 3 x PLAYER_H
#define MAZE_ROOM_W 488
#define MAZE_WALL   20
void scene_level(World *w);

// Where the player starts in the level: the middle of a room near the centre of the world.
void scene_level_start(const World *w, int *x, int *y);

// Puts the player (PLAYER_W x PLAYER_H) standing on whatever is below column x (the first cell
// from the top that is not void or gas). Returns its creature index, or -1.
int scene_spawn_player(World *w, int x);

// The same, searching down from row y (which must be in open space) instead of from the top.
int scene_spawn_player_from(World *w, int x, int y);

// The empty box plus some rock and iron, a pool of water and pockets of each gas.
void scene_demo(World *w);

// The benchmark scenario: a rock floor (bottom quarter), and above it a band of random churning
// mix of water, gases and vacuum filling `fill` (0..1) of the space up to the ceiling; the rest is
// vacuum. Deterministic for a given world seed.
void scene_bench(World *w, float fill);
