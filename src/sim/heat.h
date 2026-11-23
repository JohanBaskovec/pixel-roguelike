#pragma once

// Heat (M3): conduction and phase changes. docs/SIMULATION.md -> Heat pass. Internal to src/sim.

#include "sim/world.h"

typedef struct HeatStats {
    uint32_t chunks;        // chunks whose temperatures were computed this tick
    uint32_t phase_changes; // cells that melted, froze, boiled or condensed
} HeatStats;

// Runs one tick of conduction over heat-active chunks (and their neighbours, so energy is
// conserved across chunk borders), then applies phase changes. Wakes the movement, rendering and
// water pressure of what changes. Call at the start of a tick, before the movement pass.
void heat_update(World *w, HeatStats *stats);

// Frees the heat pass's scratch memory (world_destroy calls it).
void heat_free(World *w);
