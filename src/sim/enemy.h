#pragma once

// Monsters, damage and blood (M6b). docs/SIMULATION.md -> Monsters and blood.
//
// A monster is a creature (creature.h) whose body is a box of body cells: a walker (6 x 6 cells,
// 30 cm) that runs and jumps after the player, a flyer (11 x 11, 55 cm) that flies at the player.
// Both notice the player when they see it (a straight line of sight) while it is on the screen
// (World.view_w x view_h around the player), then chase it, walls or not (there is no
// pathfinding), until it is off the screen again; they bite when they touch it, and have hit
// points. A hit takes hit points and scatters blood (droplets: free particles of ELEM_BLOOD that
// land as liquid). A dead body stays where it fell and bleeds on for a while.

#include "sim/creature.h"

#define WALKER_W      6
#define WALKER_H      6
#define WALKER_HP     40.0f
#define WALKER_SPEED  (2.5f / CELL_SIZE_M * TICK_SECONDS) // cells per tick
#define WALKER_BITE   10.0f
#define FLYER_W       11
#define FLYER_H       11
#define FLYER_HP      30.0f
#define FLYER_SPEED   (3.0f / CELL_SIZE_M * TICK_SECONDS)
#define FLYER_BITE    6.0f
#define BITE_TICKS    40 // between bites

// Puts a monster (KIND_WALKER or KIND_FLYER) with its top-left cell at (x, y). Returns its
// creature index, or -1 (no room: the area must hold only void and gas).
int enemy_spawn(World *w, int kind, int x, int y);

// One tick of a live monster's mind, before it moves: chase or wander, and bite.
void enemy_think(World *w, Creature *c);

// The creature whose body holds cell (x, y), or -1.
int creature_at(const World *w, int x, int y);

// `c` takes `hp` hit points (it dies at 0) and blood flies from cell (x, y) (a free cell next to
// where it was hit) along (ux, uy), a unit vector. Dead bodies take no damage but still bleed.
void creature_hit(World *w, Creature *c, float hp, int x, int y, float ux, float uy);

// Throws `n` droplets of blood from cell (x, y), mostly along (ux, uy). Nothing if the cell is not
// free.
void blood_splash(World *w, int x, int y, float ux, float uy, int n, uint64_t seed);

// A dead body loses a droplet now and then, until its blood (`bleed`) is gone.
void enemy_bleed(World *w, Creature *c);
