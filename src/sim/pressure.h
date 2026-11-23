#pragma once

// Water pressure (M2b). docs/SIMULATION.md -> Liquid pressure; docs/DECISIONS.md D-018.
//
// Still liquid (liquid that is not falling) is grouped into connected bodies. Each body knows its
// top: the highest "head" among its free-surface cells, where a surface cell's head is its row
// minus the pressure of the gas above it, expressed as a height of the liquid (liquid_head; 1 atm
// on water is 207 cells). A still cell next to an open cell (void or lighter fluid) whose head
// (row minus its gas pressure) is at least one cell below that top is pushed: one cell of liquid
// moves from the body's top surface to the opening. That levels pools over any distance, balances
// U-tubes, drains reservoirs through openings, and lets gas pressure push on liquids (a
// pressurised tank drives water up a pipe; water rises 207 cells into a vacuum: a barometer).
//
// Bodies are labelled per chunk (only chunks where liquid changed, in parallel) and joined across
// chunk borders with a union-find over the per-chunk pieces every tick, so the result is exact:
// no stale values, no distance limit. Internal to src/sim.

#include "sim/world.h"

#define PRESSURE_NO_TOP INT32_MAX // body without a free surface (closed), or not still liquid

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

// Top (smallest head, see above) of the body the still cell at (x, y) belongs to, or
// PRESSURE_NO_TOP. Valid for the positions labelled at the start of the tick.
int32_t pressure_body_top(const World *w, int x, int y);

// Row of the free-surface cells that make that top, or PRESSURE_NO_TOP.
int32_t pressure_body_top_row(const World *w, int x, int y);

// Carries out requests in order, serially: take a free-surface cell from the top of P's body and
// move it to Q. Gas at Q is squeezed into a neighbouring cell of the same gas if there is one
// (the source cell is left empty), else trades places with the moving cell. Returns the number of
// cells moved. Wakes what it changes.
int pressure_apply(World *w, const PressureRequest *req, int count);
