#pragma once

// Water pressure (M2b). docs/SIMULATION.md -> Liquid pressure; docs/DECISIONS.md D-018.
//
// Still liquid (liquid that is not falling) is grouped into connected bodies. Each body knows its
// highest free-surface row. A still cell next to an open cell (void or lighter fluid) at least one
// row below that top is pushed: one cell of liquid moves from the body's top row to the opening.
// That levels pools over any distance, balances U-tubes and drains reservoirs through openings.
//
// Bodies are labelled per chunk (only chunks where liquid changed, in parallel) and joined across
// chunk borders with a union-find over the per-chunk pieces every tick, so the result is exact:
// no stale values, no distance limit. Internal to src/sim.

#include "sim/world.h"

#define PRESSURE_NO_TOP INT16_MAX // body without a free surface (closed), or not still liquid

// A still-liquid cell at P with an open neighbour at Q asks for one cell to be moved to Q.
typedef struct PressureRequest {
    int32_t px, py, qx, qy;
} PressureRequest;

typedef struct PressureState PressureState;

PressureState *pressure_create(const World *w);
void pressure_destroy(PressureState *ps);

// Relabels chunks flagged head_dirty, rebuilds the bodies, and wakes chunks whose body top rose
// (their cells may now be pushed). Call at the start of a tick.
void pressure_update(World *w);

// Highest free-surface row (smallest y) of the body the still cell at (x, y) belongs to, or
// PRESSURE_NO_TOP. Valid for the positions labelled at the start of the tick.
int pressure_body_top(const World *w, int x, int y);

// Carries out requests in order, serially: take a free-surface cell from the top row of P's body
// and move it to Q. Returns the number of cells moved. Wakes what it changes.
int pressure_apply(World *w, const PressureRequest *req, int count);
