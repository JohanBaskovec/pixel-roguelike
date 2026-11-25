#pragma once

// Saving and loading a game (M6b, D-048). docs/ARCHITECTURE.md -> Saves.
//
// One file holds the world (every cell: its element, aux and shade, its temperature, and the gas
// of the gas cells, the three of them run-length encoded: settled matter is long runs), the free
// particles, the creatures, the crates and the rigid bodies, and the player's index and gun.
// It is a dump for testing, not a format to keep: it holds the memory layout of the structs
// (checked by sizes, so a save from another build of the game is refused), and the byte order of
// the machine. What can be worked out is not saved (water bodies, chunk activity, the bullets in
// flight's payloads): everything is woken on load.

#include "sim/weapon.h"

#include <stdio.h>

// What the game adds to the world's state.
typedef struct SaveExtra {
    int32_t player; // the player's creature index (-1: none)
    Rifle rifle;    // the gun, with the bullet designs
} SaveExtra;

// Writes the world and `extra` to f. Returns false on a write error.
bool world_save(const World *w, FILE *f, const SaveExtra *extra);

// Reads a save into w (which must be of the same size) and `extra`. Returns false, with w cleared
// (a new empty world: the caller builds a level again) if the file is not a save of this build, is
// of another size, or is cut short.
bool world_load(World *w, FILE *f, SaveExtra *extra);
