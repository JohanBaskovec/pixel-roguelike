#pragma once

// Rigid objects (M6b, D-045): bodies made of many pixels that fall, tip, slide and rest, and are
// cut into pieces by what removes their pixels (bullets, the brush, fire). docs/SIMULATION.md ->
// Rigid objects.
//
// A body owns its pixels (element, temperature, burn state) in a small bitmap, with a pose: the
// position of its centre of mass, an angle, a velocity and a spin. Each tick the grid shows it:
// the world cells whose centres fall on its pixels (the pose, rotated, sampled backwards so there
// are no holes) hold its pixels, as wood cells that matter, heat and fire treat like any other;
// what a cell it moves into held (gas) is swapped into a cell it leaves, so nothing is lost. The
// body moves in sub-steps of at most a cell, stops where it would overlap anything solid (the
// terrain, creatures, other bodies, liquid), and answers a blow there with an impulse at the
// contact point (restitution, friction), so it tips and rolls. At rest it sleeps.
//
// A pixel whose cell is no longer wood (shot, erased, burnt away) is removed; the rest is split
// into connected pieces, each a new body that keeps the velocity of its own part. Pieces under
// RIGID_MIN_PIECE pixels vanish. Bodies are updated serially at the start of the tick, outside
// the simulated area they are frozen.

#include "sim/world.h"

#define RIGID_MAX        128
#define RIGID_MAX_DIM    64   // bitmap side, pixels
#define RIGID_MAX_CELLS  1536 // world cells one body can fill
#define RIGID_MIN_PIECE  2

// What each pixel holds: a wood cell (ELEM_WOOD or ELEM_BURNING_WOOD) and its temperature.
// cell.element == ELEM_VOID: no pixel.
typedef struct RigidPixel {
    Cell cell;
    float temp;
} RigidPixel;

typedef struct Rigid {
    uint8_t active;
    uint8_t asleep;
    int16_t w, h;       // bitmap size
    int32_t pixels;     // filled pixels; also the mass, in pixel masses
    float cx, cy;       // centre of mass in bitmap coordinates (pixel (i, j) is centred at i + .5, j + .5)
    float x, y;         // the centre of mass in the world, cells
    float angle;        // radians; +x turns toward +y (clockwise on the screen)
    float vx, vy;       // cells per tick
    float omega;        // radians per tick
    float inertia;      // about the centre of mass, in pixel masses x cells^2
    float radius;       // farthest a pixel reaches from the centre of mass
    int32_t rest_ticks; // ticks it has hardly moved
    int32_t ncells;     // world cells it fills now
    int32_t cell_idx[RIGID_MAX_CELLS];  // their grid indices
    uint16_t cell_pix[RIGID_MAX_CELLS]; // the pixel (j * w + i) each shows
    RigidPixel pix[RIGID_MAX_DIM * RIGID_MAX_DIM];
} Rigid;

// A rectangle of wood (bw x bh pixels: a log when it is long and thin), its centre at (cx, cy),
// turned by `angle`. Needs void or gas under it. Returns its index, or -1.
int rigid_spawn_box(World *w, float cx, float cy, int bw, int bh, float angle);

// Moves every awake body one tick and splits those that lost pixels. sim_tick calls it first.
void rigids_update(World *w);

// The body that fills cell (x, y), or -1.
int rigid_at(const World *w, int x, int y);

// A blow at cell (x, y) along the unit vector (ux, uy) with `momentum` (kg m/s): wakes the body
// there and kicks it (at the contact point, so it spins).
void rigid_hit(World *w, int x, int y, float ux, float uy, float momentum);

// Frees the bodies (world_destroy).
void rigids_free(World *w);
