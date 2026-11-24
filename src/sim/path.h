#pragma once

// Pathfinding for monsters (M6b; D-043). docs/SIMULATION.md -> Pathfinding.
//
// The world around the player (twice the screen, at most PATH_BLOCKS_X x PATH_BLOCKS_Y blocks) is
// sampled in blocks of PATH_BLOCK cells: a block is open if its centre cell holds void, gas,
// liquid or a body (a wall at least a block thick always shows). From the player's block, two
// breadth-first searches give every open block its distance to the player: one for flyers (any
// move to an open neighbour) and one for walkers, who may walk to a side neighbour, fall to the
// block below and step up one block onto a ledge, but not climb. The fields are rebuilt every
// PATH_TICKS ticks, when a monster asks, so digging changes the routes; a monster follows the
// descent of the field. Serial use only (creatures update serially).

#include "sim/world.h"

#define PATH_BLOCK    6   // cells: the size of a walker
#define PATH_TICKS    20  // between rebuilds
#define PATH_BLOCKS_X 320 // largest window, blocks
#define PATH_BLOCKS_Y 240

// Rebuilds the fields toward the player at cell (px, py) if they are older than PATH_TICKS (or
// built for another world tick sequence). Returns false if the player's block is not open.
bool path_update(World *w, int px, int py);

// A flyer centred on cell (cx, cy): the cell to fly toward next, along the descent of the field.
// False if the field does not cover it or it cannot reach the player (then fly straight at it).
bool path_flyer_step(const World *w, int cx, int cy, float *tx, float *ty);

// A walker centred on cell (cx, cy): the side to walk (-1, 0, 1) and whether to jump, along the
// descent of the walker field. False if it cannot reach the player by walking.
bool path_walker_step(const World *w, int cx, int cy, int *walk, int *jump);

// Frees the fields (world_destroy).
void path_free(World *w);
