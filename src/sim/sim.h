#pragma once

#include "sim/world.h"

typedef struct SimStats {
    uint64_t cells_visited; // movable cells examined inside dirty rects
    uint64_t cells_moved;   // successful moves/swaps
    uint32_t chunks_active; // chunks with a non-empty dirty rect this tick
} SimStats;

// Advances the world by one fixed tick (1/60 s). Fills `stats` if non-NULL.
void sim_tick(World *w, SimStats *stats);

// Runs n ticks (tests, benchmarks).
void sim_run_ticks(World *w, int n);
