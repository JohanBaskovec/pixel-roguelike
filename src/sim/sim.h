#pragma once

#include "sim/world.h"

typedef struct SimStats {
    uint64_t cells_visited; // movable cells examined inside dirty rects
    uint64_t cells_moved;   // successful moves/swaps
    uint32_t chunks_active; // chunks with a non-empty dirty rect this tick
    uint32_t transfers;     // cells moved by water pressure (M2b)
    uint32_t requests;      // water-pressure requests (cells pushed toward an opening)
    uint32_t heat_chunks;   // chunks whose temperatures were computed (M3)
    uint32_t phase_changes; // cells that melted, froze, boiled or condensed
    uint32_t gas_transfers; // gas pressure equalisations between two cells (M4)
    uint32_t particles;     // free particles in flight after the tick (M4b)
    uint32_t launched;      // particles launched this tick (jets and splashes)
    uint32_t reactions;     // reactions this tick (M5)
} SimStats;

// Advances the world by one fixed tick (1/60 s). Fills `stats` if non-NULL.
void sim_tick(World *w, SimStats *stats);

// Runs n ticks (tests, benchmarks).
void sim_run_ticks(World *w, int n);

// Water pressure at (x, y) as depth in cells below the top surface of the body of still liquid
// the cell belongs to; -1 if it is not still liquid (or the body has no free surface). As
// labelled at the start of the last tick. For overlays and inspection.
int sim_pressure_depth(const World *w, int x, int y);

// Absolute pressure (Pa) in the still liquid at (x, y): the gas pressure on its body's surface
// plus the weight of the liquid above. -1 if it is not still liquid or its body has no free
// surface. Resolution: one cell of liquid (490 Pa for water).
float sim_liquid_pressure(const World *w, int x, int y);
