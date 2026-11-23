#pragma once

// Free particles (M4b): matter that leaves the grid while it flies fast (jets, splashes), as in
// Noita, and lands back into it. docs/SIMULATION.md -> Free particles; docs/DECISIONS.md D-026.
//
// Particles are not on the grid: the cell they came from is empty while they fly, and they pass
// through void and gas. They are updated serially, in list order, at the end of each tick, so the
// result does not depend on the number of threads.

#include "sim/world.h"

#define PARTICLE_MAX (1 << 17) // a full list refuses new particles (jets fall back to cells)

// Gravity in cells per tick^2: 9.81 m/s^2 at 5 cm per cell and 60 ticks/s.
#define PARTICLE_GRAVITY (GRAVITY / CELL_SIZE_M * TICK_SECONDS * TICK_SECONDS)
// Speed limit, cells per tick (about 30 m/s); also keeps the sub-step loop short.
#define PARTICLE_MAX_SPEED 24.0f

typedef struct Particle {
    float x, y;   // position in cells: cell (i, j) covers [i, i + 1) x [j, j + 1)
    float vx, vy; // velocity, cells per tick (+y is down)
    Cell cell;    // what it carries (a liquid)
    float temp;   // its stored temperature
} Particle;

// Adds a particle. Its cell must be free (void or gas). Returns false if the list is full.
bool particle_add(World *w, const Particle *p);

// Moves a particle by `fraction` of a tick at its velocity. If it hits something it lands (back
// on the grid) and true is returned; the caller removes it. Serial use only.
bool particle_step(World *w, Particle *p, float fraction);

// Lands the particle where it is (or the nearest cell above that passes). Used when the list is
// full. Returns false if there was no room (then the caller must keep it).
bool particle_land_now(World *w, Particle *p);

// Gravity and one tick of flight for every particle; landed ones are removed. Call at the end of
// the tick, after the movement pass.
void particles_update(World *w);

// Can a particle land at (x, y)? Void or gas.
static inline bool particle_passes(const World *w, int x, int y) {
    uint8_t b = g_elem_behavior[world_elem(w, x, y)];
    return b == BEHAVIOR_NONE || b == BEHAVIOR_GAS;
}

// Can a particle fly through (x, y)? Void, gas, or falling liquid (a jet crosses a thin falling
// stream instead of stopping in it).
static inline bool particle_flies_through(const World *w, int x, int y) {
    if (!world_in_bounds(w, x, y)) {
        return false;
    }
    Cell c = w->cells[world_index(w, x, y)];
    uint8_t b = g_elem_behavior[c.element];
    return b == BEHAVIOR_NONE || b == BEHAVIOR_GAS || (b == BEHAVIOR_LIQUID && (c.aux & 0x7F));
}
