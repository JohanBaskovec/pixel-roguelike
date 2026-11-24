// Movement pass. Rules are documented in docs/SIMULATION.md; keep them in sync.
//
// HOT PATH: everything below runs per cell per tick. No allocation, logging, SDL calls, locks
// or function pointers. Respect MAX_MOVE.
//
// THREADING: the chunks of one checkerboard phase are updated in parallel, one job per chunk.
// A job reads and writes cells only within MAX_MOVE of its own chunk (asserted on every swap in
// Debug), so jobs of the same phase never touch the same cells. Wakes for the next tick go into
// the job's own TickCtx (the 3x3 chunks around its chunk) and are merged into the chunks after
// the phase. See docs/ARCHITECTURE.md -> Parallel movement.

#include "sim/sim.h"

#include "core/jobs.h"
#include "core/rng.h"
#include "sim/creature.h"
#include "sim/heat.h"
#include "sim/particle.h"
#include "sim/pickup.h"
#include "sim/pressure.h"
#include "sim/rigid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MAX_REQUESTS 128 // water-pressure requests per chunk per tick; more retry next tick
#define TICK_MAX_SPAWNS   8   // particles a chunk may launch per tick (splashes)

// Splashes (M4b): falling liquid that lands at least this fast (1/16 cells per tick; 32 = 2 cells
// per tick, 6 m/s, a fall of about 1.8 m) is thrown off as a particle with this probability
// (x/65536), keeping part of its speed: sideways SPLASH_KEEP_MIN..MAX of it, upward up to
// SPLASH_UP of it.
#define SPLASH_MIN_SPEED 32
#define SPLASH_P16       16384u
#define SPLASH_KEEP_MIN  0.25f
#define SPLASH_KEEP_MAX  0.6f
#define SPLASH_UP        0.25f
// Splashes are allowed only while the particle list is below this fill.
#define SPLASH_LIST_LIMIT (PARTICLE_MAX / 2)

// Convection: swap probability per unit of relative density difference (a liquid 0.25% lighter
// than the same liquid above it, e.g. water 10 K warmer at 25 degC, rises with p = 0.5).
#define K_CONVECTION 200.0f
// Smallest temperature difference that drives convection or same-gas buoyancy.
#define CONVECTION_MIN_K 0.05f
// Temperatures closer than this count as equal when deciding whether moving cells need heat.
#define HEAT_UNIFORM_K 0.01f

// Per-job state. Also holds the job's results (wakes, stats) until they are merged.
typedef struct TickCtx {
    World *w;
    Rng rng;
    uint8_t stamp;
    int cx, cy;                 // the chunk this job updates
    int base_x, base_y;         // its top-left cell
    DirtyRect wake[9];          // next-tick wakes for the 3x3 chunks around it (chunk-local coords)
    uint8_t render[9];          // ... and whether they need re-rendering
    uint8_t liquid[9];          // ... and whether liquid moved there (recompute water pressure)
    float arr_lo[9], arr_hi[9]; // temperatures of cells that moved into those chunks
    SimStats stats;
    int nreq; // water-pressure requests, carried out after the phase
    PressureRequest req[TICK_MAX_REQUESTS];
    bool splash_ok; // the particle list has room for splashes this tick
    bool may_react; // something around the chunk is hot enough to ignite a reaction
    int nspawn;     // particles launched by this job, added to the list after the phase
    Particle spawn[TICK_MAX_SPAWNS];
} TickCtx;

// Mark a world-space rect, which must lie in the 3x3 chunks around the job's chunk.
static void ctx_wake_slow(TickCtx *c, int x0, int y0, int x1, int y1, bool render, bool liquid) {
    World *w = c->w;
    x0 = PR_MAX(x0, 0);
    y0 = PR_MAX(y0, 0);
    x1 = PR_MIN(x1, w->width - 1);
    y1 = PR_MIN(y1, w->height - 1);
    for (int ccy = y0 >> CHUNK_SHIFT; ccy <= y1 >> CHUNK_SHIFT; ccy++) {
        int by = ccy << CHUNK_SHIFT;
        for (int ccx = x0 >> CHUNK_SHIFT; ccx <= x1 >> CHUNK_SHIFT; ccx++) {
            int bx = ccx << CHUNK_SHIFT;
            int slot = (ccx - c->cx + 1) + 3 * (ccy - c->cy + 1);
            PR_ASSERT(ccx - c->cx >= -1 && ccx - c->cx <= 1 && ccy - c->cy >= -1 &&
                      ccy - c->cy <= 1);
            dirty_rect_add(&c->wake[slot], PR_MAX(x0 - bx, 0), PR_MAX(y0 - by, 0),
                           PR_MIN(x1 - bx, CHUNK_SIZE - 1), PR_MIN(y1 - by, CHUNK_SIZE - 1));
            c->render[slot] |= (uint8_t)render;
            c->liquid[slot] |= (uint8_t)liquid;
        }
    }
}

static inline void ctx_wake(TickCtx *c, int x0, int y0, int x1, int y1, bool render, bool liquid) {
    int lx0 = x0 - c->base_x, ly0 = y0 - c->base_y;
    int lx1 = x1 - c->base_x, ly1 = y1 - c->base_y;
    if (PR_LIKELY(lx0 >= 0 && ly0 >= 0 && lx1 < CHUNK_SIZE && ly1 < CHUNK_SIZE)) {
        dirty_rect_add(&c->wake[4], lx0, ly0, lx1, ly1);
        c->render[4] |= (uint8_t)render;
        c->liquid[4] |= (uint8_t)liquid;
    } else {
        ctx_wake_slow(c, x0, y0, x1, y1, render, liquid);
    }
}

// A cell with temperature t moved to (x, y): remember it for the destination chunk's heat check.
static inline void ctx_arrive(TickCtx *c, int x, int y, Cell cell, float t) {
    if (g_elem_cap[cell.element] == 0.0f) {
        return;
    }
    int slot = ((x >> CHUNK_SHIFT) - c->cx + 1) + 3 * ((y >> CHUNK_SHIFT) - c->cy + 1);
    PR_ASSERT(slot >= 0 && slot < 9);
    float tp = elem_temp(cell.element, t);
    c->arr_lo[slot] = PR_MIN(c->arr_lo[slot], tp);
    c->arr_hi[slot] = PR_MAX(c->arr_hi[slot], tp);
}

// The cell at (x, y) did not change but may move next tick (e.g. a probabilistic move failed):
// keep only that cell awake, no re-render.
static inline void keep_alive(TickCtx *c, int x, int y) {
    ctx_wake(c, x, y, x, y, false, false);
}

#ifndef NDEBUG
// Is (x, y) within the cells this job may write: its chunk plus MAX_MOVE on every side?
static inline bool in_reach(const TickCtx *c, int x, int y) {
    return x >= c->base_x - MAX_MOVE && x < c->base_x + CHUNK_SIZE + MAX_MOVE &&
           y >= c->base_y - MAX_MOVE && y < c->base_y + CHUNK_SIZE + MAX_MOVE;
}
#endif

// Gravity: liquids gain 1/16 px/tick of fall speed with this probability (x/65536) each tick.
// 0.87/16 px/tick^2 * 60^2 * 0.05 m = 9.8 m/s^2 at 5 cm per pixel. Random rather than on fixed
// ticks: cells released together would otherwise fall as rigid horizontal lines.
#define GRAVITY_INC_P16 57000u
#define FALL_SPEED_MAX  127 // 1/16 px/tick units; ~8 px/tick

// How far a liquid looks sideways for somewhere to fall. Must not exceed MAX_MOVE: rules may only
// read and write within MAX_MOVE of the cell (threading invariant).
#define LIQUID_LOOKAHEAD MAX_MOVE

// Falling matter stirs the gas it passes through (gas currents, defined with the gas rules).
static void stir_by_fall(TickCtx *c, int x, int y, int target, int speed);

static inline Cell *cell_ptr(World *w, int x, int y) {
    return &w->cells[world_index(w, x, y)];
}

// Swaps two cells (with their temperatures and masses) up to MAX_MOVE apart. Multi-cell moves are
// done as one swap from start to destination: the cells passed over are void (unchanged by the
// move) or a lighter fluid that simply trades places with the mover.
static inline void swap_cells(TickCtx *c, int x0, int y0, int x1, int y1) {
    World *w = c->w;
    PR_ASSERT(world_in_bounds(w, x0, y0) && world_in_bounds(w, x1, y1));
    PR_ASSERT(x1 - x0 <= MAX_MOVE && x0 - x1 <= MAX_MOVE && y1 - y0 <= MAX_MOVE &&
              y0 - y1 <= MAX_MOVE);
    PR_ASSERT(in_reach(c, x0, y0) && in_reach(c, x1, y1));
    size_t i0 = world_index(w, x0, y0);
    size_t i1 = world_index(w, x1, y1);
    Cell a = w->cells[i0];
    Cell b = w->cells[i1];
    a.stamp = c->stamp;
    b.stamp = c->stamp;
    w->cells[i0] = b;
    w->cells[i1] = a;
    float t = w->temp[i0];
    w->temp[i0] = w->temp[i1];
    w->temp[i1] = t;
    GasMix g = w->gas[i0];
    w->gas[i0] = w->gas[i1];
    w->gas[i1] = g;
    float r = w->rho[i0]; // the cached densities follow (buoyancy reads them this tick)
    w->rho[i0] = w->rho[i1];
    w->rho[i1] = r;
    ctx_arrive(c, x1, y1, a, t);           // a moved to (x1, y1)
    ctx_arrive(c, x0, y0, b, w->temp[i0]); // b moved to (x0, y0)
    // Both cells changed: wake their 3x3 neighbourhoods (as one rect) and re-render.
    // Still liquid moved (or started to fall): its body must be relabelled for water pressure.
    // Falling liquid passing through does not change any body.
    // Two still cells of the same liquid trading places (convection) leave the bodies unchanged.
    bool sa = cell_is_still_liquid(a), sb = cell_is_still_liquid(b);
    bool liquid = (sa || sb) && !(sa && sb && a.element == b.element);
    ctx_wake(c, PR_MIN(x0, x1) - 1, PR_MIN(y0, y1) - 1, PR_MAX(x0, x1) + 1, PR_MAX(y0, y1) + 1,
             true, liquid);
    c->stats.cells_moved++;
}

// Can a cell of density `d` move into (x, y), going down or sideways? Void, or a lighter fluid.
static inline bool can_displace(const World *w, int x, int y, uint32_t d) {
    ElementId t = world_elem(w, x, y);
    uint8_t b = g_elem_behavior[t];
    if (b == BEHAVIOR_NONE) {
        return true;
    }
    return b >= BEHAVIOR_LIQUID && g_elem_density_q[t] < d;
}

static inline bool is_blocking(const World *w, int x, int y) {
    return g_elem_behavior[world_elem(w, x, y)] == BEHAVIOR_STATIC;
}

// A diagonal step (dx, dy) may not squeeze between two blocking cells (no leaking through
// 8-connected walls).
static inline bool diagonal_open(const World *w, int x, int y, int dx, int dy) {
    return !(is_blocking(w, x + dx, y) && is_blocking(w, x, y + dy));
}

// A cell of liquid `e` that is falling (non-zero fall speed).
static inline bool liquid_in_transit(const World *w, int x, int y, ElementId e) {
    if (!world_in_bounds(w, x, y)) {
        return false;
    }
    Cell t = w->cells[world_index(w, x, y)];
    return t.element == e && (t.aux & 0x7F) != 0;
}

// Is the cell below (x, y) part of a falling column of liquid `e` that still has room to fall?
// Looks down through falling cells of `e`, within MAX_MOVE, for an open cell.
static inline bool falling_column_below(const World *w, int x, int y, ElementId e, uint32_t d) {
    for (int ny = y + 1; ny <= y + MAX_MOVE; ny++) {
        if (can_displace(w, x, ny, d)) {
            return ny > y + 1;
        }
        if (!liquid_in_transit(w, x, ny, e)) {
            return false;
        }
    }
    return false;
}

static inline bool liquid_try_diagonal(TickCtx *c, int x, int y, uint32_t d) {
    World *w = c->w;
    int first = rng_bit(&c->rng) ? 1 : -1;
    for (int k = 0; k < 2; k++) {
        int dx = k == 0 ? first : -first;
        if (can_displace(w, x + dx, y + 1, d) && diagonal_open(w, x, y, dx, 1)) {
            swap_cells(c, x, y, x + dx, y + 1);
            cell_ptr(w, x + dx, y + 1)->aux = (uint8_t)(dx > 0 ? 0x80 : 0);
            return true;
        }
    }
    return false;
}

static void update_liquid(TickCtx *c, int x, int y) {
    World *w = c->w;
    Cell *cell = cell_ptr(w, x, y);
    uint32_t d = g_elem_density_q[cell->element];
    int speed = cell->aux & 0x7F;
    int dir = (cell->aux & 0x80) ? 1 : -1;

    // 1. Fall, accelerating under gravity. A cell whose way down is blocked by its own liquid
    //    that is falling, with open space further down, waits in mid-air (keeping its speed)
    //    instead of landing: that column is moving. This happens where the chunk below is
    //    scanned after this one, and landing there made a falling stream bulge below every chunk
    //    border. If the falling column below ends on something that does not move, it is a real
    //    jam (a stream pouring in faster than the pool drains), and the cell lands and spreads as
    //    usual; waiting there would stack the stream into a standing tower.
    bool below_open = can_displace(w, x, y + 1, d);
    if (below_open || falling_column_below(w, x, y, cell->element, d)) {
        if (speed < FALL_SPEED_MAX && rng_chance16(&c->rng, GRAVITY_INC_P16)) {
            speed++;
        }
        int steps = 1 + (speed >> 4);
        int target = 0;
        for (int ny = y + 1; ny <= y + MAX_MOVE && steps > 0; ny++) {
            if (can_displace(w, x, ny, d)) {
                target = ny;
                steps--;
            } else {
                break;
            }
        }
        if (target == 0) {
            cell->aux = (uint8_t)((dir > 0 ? 0x80 : 0) | speed);
            keep_alive(c, x, y); // jammed behind falling liquid: wait, still falling
            return;
        }
        // A falling cell always has a non-zero speed: only still liquid carries pressure, and a
        // stream must not count as part of the pool it pours into.
        speed = PR_MAX(speed, 1);
        swap_cells(c, x, y, x, target);
        cell_ptr(w, x, target)->aux = (uint8_t)((dir > 0 ? 0x80 : 0) | speed);
        if (g_elem_behavior[world_elem(w, x, y)] == BEHAVIOR_GAS) {
            stir_by_fall(c, x, y, target, speed);
        }
        return;
    }

    // Landing: fall speed turns into extra sideways spread this tick, and is reset, so a resting
    // cell never counts as "in transit". A landed cell joins a body of still water.
    //    Landing fast, it may splash: thrown off the grid as a particle that keeps part of its
    //    speed, sideways and a little upward.
    if (speed >= SPLASH_MIN_SPEED && c->splash_ok && c->nspawn < TICK_MAX_SPAWNS &&
        rng_chance16(&c->rng, SPLASH_P16)) {
        float s = (float)speed / 16.0f;
        float u = (float)(rng_next(&c->rng) >> 8) * (1.0f / 16777216.0f);
        float v = (float)(rng_next(&c->rng) >> 8) * (1.0f / 16777216.0f);
        float side = rng_bit(&c->rng) ? 1.0f : -1.0f;
        size_t i = world_index(w, x, y);
        Cell flying = *cell;
        flying.aux = 0;
        c->spawn[c->nspawn++] = (Particle){
            (float)x + 0.5f,
            (float)y + 0.5f,
            side * s * (SPLASH_KEEP_MIN + (SPLASH_KEEP_MAX - SPLASH_KEEP_MIN) * u),
            -s * SPLASH_UP * v,
            flying,
            w->temp[i],
        };
        *cell = (Cell){ELEM_VOID, c->stamp, cell->shade, 0};
        w->temp[i] = 0.0f;
        gas_mix_clear(&w->gas[i]);
        ctx_wake(c, x - 1, y - 1, x + 1, y + 1, true, false);
        return;
    }
    int spread = g_elem_spread[cell->element] + (speed >> 4);
    spread = PR_MIN(spread, MAX_MOVE);
    cell->aux = (uint8_t)(dir > 0 ? 0x80 : 0);
    if (speed > 0) {
        ctx_wake(c, x, y, x, y, false, true);
    }

    // 2. Water pressure: if the body of still water this cell belongs to has its top at least
    //    one cell above an open cell above or beside this cell, ask for one cell of water to be
    //    moved from that top surface to the open cell (carried out after the phase, see
    //    pressure.c). Heights count what presses on the liquid (as a height of it): the body's
    //    top is raised by lighter liquids and gas on its surface, the open cell by those in and
    //    above it. This levels
    //    pools over any distance, balances U-tubes, makes deep water push out of openings and
    //    lets gas push on water.
    bool labelled = w->piece[world_index(w, x, y)] != 0;
    int32_t body_top = labelled ? pressure_body_top(w, x, y) : PRESSURE_NO_TOP;
    if (body_top != PRESSURE_NO_TOP) {
        const int q[3][2] = {{x, y - 1}, {x + dir, y}, {x - dir, y}};
        for (int k = 0; k < 3; k++) {
            int qx = q[k][0], qy = q[k][1];
            if (body_top <= qy - 1 && can_displace(w, qx, qy, d) &&
                // A first check; within reach (the transfer counts the whole column above Q).
                body_top <= qy - 1 - pressure_load_head(w, qx, qy, cell->element, MAX_MOVE - 1)) {
                if (c->nreq < TICK_MAX_REQUESTS) {
                    c->req[c->nreq++] = (PressureRequest){x, y, qx, qy};
                } else {
                    keep_alive(c, x, y); // no room this tick: retry next tick
                }
                return;
            }
        }
    }

    // 3. Convection: if the same liquid sits above this cell and is denser (for water: colder,
    //    unless near 4 degC), the two trade places with a probability that grows with the
    //    difference. Warm water rises, cold water sinks, and heat is carried with them.
    if (y > 0) {
        Cell above = w->cells[world_index(w, x, y - 1)];
        if (above.element == cell->element && cell_is_still_liquid(above)) {
            float ts = elem_temp(cell->element, w->temp[world_index(w, x, y)]);
            float ta = elem_temp(cell->element, w->temp[world_index(w, x, y - 1)]);
            if (fabsf(ts - ta) >= CONVECTION_MIN_K) {
                float ds = elem_fluid_density(cell->element, ts);
                float da = elem_fluid_density(cell->element, ta);
                if (da > ds) {
                    float p = PR_MIN(K_CONVECTION * (da - ds) / da, 1.0f);
                    if (rng_chance16(&c->rng, (uint32_t)(p * 65536.0f))) {
                        swap_cells(c, x, y, x, y - 1);
                        return;
                    }
                    keep_alive(c, x, y);
                }
            }
        }
    }

    // A cell with still liquid on top of it is not at the surface: it does not flow on its own
    // (water pressure moves water to where it is pushed). Falling water on top does not count.
    bool pressured = !liquid_surface_free(w, x, y, cell->element) &&
                     g_elem_behavior[world_elem(w, x, y - 1)] == BEHAVIOR_LIQUID;

    // 3. Slide diagonally down.
    if (liquid_try_diagonal(c, x, y, d)) {
        return;
    }
    bool open_beside = can_displace(w, x - 1, y, d) || can_displace(w, x + 1, y, d);
    if (pressured) {
        // Not labelled yet (the cell just landed or moved) but open space beside: look again
        // next tick, when its body is known.
        if (!labelled && open_beside) {
            keep_alive(c, x, y);
        }
        cell->aux = (uint8_t)(dir > 0 ? 0x80 : 0);
        return;
    }

    // 4. Surface cell: flow sideways toward a drop (a cell it can fall into) seen within
    //    LIQUID_LOOKAHEAD, current direction first. It looks through its own liquid, so the whole
    //    top row of a step flows down to the next step, not just the step's edge cell (otherwise
    //    poured water piles into a stepped mound). Through open space it moves at most `spread`
    //    cells; past its own liquid it moves straight to the drop. Without a drop in sight the
    //    surface is level here; any larger difference further away is water pressure's job.
    for (int attempt = 0; attempt < 2; attempt++) {
        int land = 0;
        bool passed_own = false;
        for (int s = 1; s <= LIQUID_LOOKAHEAD; s++) {
            int nx = x + dir * s;
            if (!can_displace(w, nx, y, d)) {
                if (world_elem(w, nx, y) == cell->element) {
                    passed_own = true;
                    continue;
                }
                break;
            }
            if (can_displace(w, nx, y + 1, d)) {
                land = passed_own ? s : PR_MIN(s, spread);
                break;
            }
        }
        if (land > 0) {
            int cx = x + dir * land;
            swap_cells(c, x, y, cx, y);
            cell_ptr(w, cx, y)->aux = (uint8_t)(dir > 0 ? 0x80 : 0);
            return;
        }
        dir = -dir;
    }
    if (!labelled && open_beside) {
        keep_alive(c, x, y);
    }
    // Settle. No wake, so the cell sleeps.
    cell->aux = (uint8_t)(dir > 0 ? 0x80 : 0);
}

// ---- Gases (M4; mixtures, D-035). docs/SIMULATION.md -> Gases ----
//
// A gas cell (ELEM_GAS) holds a mass of each gas species (GasMix) and one temperature. Per tick
// it may: follow its current; burn (fuel and oxidiser in the cell); condense water vapour below its
// dew point; rise through (or sink under) gas above by density; spread sideways under lighter gas
// (heavy gas pools and flattens); expand into vacuum; and, with a neighbour in one random
// direction, diffuse (trade species down their mole-fraction difference) and even out pressure
// (with a cell up to GAS_REACH away). Still, evenly mixed gas sleeps.

// Farthest gas cell a gas cell equalises its pressure with in one tick. Pressure spreads by
// diffusion; long jumps make it spread over a room in seconds instead of minutes.
#define GAS_REACH 16
// ... but only with a cell within this temperature difference (K); otherwise its neighbour.
#define GAS_JUMP_MAX_DT 2.0f
// Neighbouring gas cells count as equal (no transfer; they may sleep) when their pressures differ
// by less than GAS_EQ_TOL (relative), each species' mole fraction by less than GAS_MIX_TOL, and
// their densities (at 1 atm) by less than GAS_BUOY_TOL (relative: 0.06 K of air at 20 degC).
#define GAS_EQ_TOL     3e-3f
#define GAS_FEED_RATIO 2.1f  // see gas_equalize
#define GAS_EQ_TOL_FAR 2e-3f // for cells GAS_REACH / 2 or more apart (gas_differs_at)
#define GAS_MIX_TOL    1e-3f
#define GAS_BUOY_TOL   2e-4f
// Diffusion: a pair of neighbouring cells trades GAS_DIFFUSE_K of the difference of each species'
// mole fraction (times their mean moles) when one of them picks the other (a random direction per
// tick: each pair about every other tick). Effective diffusivity GAS_DIFFUSE_K / 2 cells^2/tick =
// 2.5e-3 m^2/s: gentle indoor turbulence, about 150 times molecular diffusion (D-032). A current
// multiplies it by 1 + GAS_TURB_PER_WIND * speed (eddy diffusivity, as for heat), up to 1/2 (the
// pair evens out).
#define GAS_DIFFUSE_K 0.0333f
// Water vapour condenses when its fraction exceeds saturation (at 1 atm total, D-035), into a
// droplet of water with probability (mass condensed) / (mass of a water cell): mass is conserved on
// average (docs/SIMULATION.md -> Phase changes). Latent heat (J/kg) is released into the gas.
#define VAPOUR_LATENT 2257e3f
// The gas a droplet displaces goes into the gas within this many cells.
#define CONDENSE_RADIUS 4
#define CONDENSE_CELLS  32

static const int8_t k_dir4[4][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};

static inline GasMix *gas_at(World *w, size_t i) {
    return &w->gas[i];
}

// Proportional to pressure (P = pv * R / V): moles times temperature.
static inline float gas_pv(const World *w, size_t i) {
    return gas_moles(&w->gas[i]) * w->temp[i];
}

// Pressures (as moles times temperature) differ by more than GAS_EQ_TOL of the higher one, and
// by more than GAS_EQ_TOL of 0.1 atm: thin gas in a vacuum (a few drifting cells) does not chase
// differences that would not move a feather.
#define GAS_PV_FLOOR (0.1f * GAS_MOLES_ATM * T_ROOM)

static inline bool gas_differs(float a, float b) {
    return fabsf(a - b) > GAS_EQ_TOL * PR_MAX(PR_MAX(a, b), GAS_PV_FLOOR);
}

// For cells d apart: GAS_EQ_TOL near, GAS_EQ_TOL_FAR from GAS_REACH / 2 cells on, so a smooth
// gradient (too gentle to show between neighbours) still evens out to GAS_EQ_TOL_FAR per
// GAS_REACH cells.
static inline bool gas_differs_at(float a, float b, int d) {
    float tol = d >= GAS_REACH / 2 ? GAS_EQ_TOL_FAR : GAS_EQ_TOL;
    return fabsf(a - b) > tol * PR_MAX(PR_MAX(a, b), GAS_PV_FLOOR);
}

// Moves the fraction f of cell a's gas into cell b, with its heat (temperatures mix by heat
// capacity: energy is conserved). Cell a keeps its temperature.
static inline void gas_move_part(World *w, size_t a, size_t b, float f) {
    GasMix part = w->gas[a];
    gas_mix_scale(&part, f);
    w->temp[b] =
        gas_mix_temp(gas_heat_cap(&w->gas[b]), w->temp[b], gas_heat_cap(&part), w->temp[a]);
    gas_mix_add(&w->gas[b], &part, 1.0f);
    gas_mix_add(&w->gas[a], &part, -1.0f);
}

// Wakes and re-renders the cells around two cells that exchanged gas (which may be up to
// GAS_REACH apart: the cells between did not change). Gas resting on liquid presses on it: if
// liquid lies below either cell, its body's top is recomputed (water pressure).
static inline void gas_wake(TickCtx *c, int x0, int y0, int x1, int y1) {
    const World *w = c->w;
    bool l0 = g_elem_behavior[world_elem(w, x0, y0 + 1)] == BEHAVIOR_LIQUID;
    bool l1 = g_elem_behavior[world_elem(w, x1, y1 + 1)] == BEHAVIOR_LIQUID;
    ctx_wake(c, x0 - 1, y0 - 1, x0 + 1, y0 + 1, true, l0);
    ctx_wake(c, x1 - 1, y1 - 1, x1 + 1, y1 + 1, true, l1);
}

// Puts a new gas cell holding g at stored temperature ts at (x, y). It counts as updated this
// tick.
static inline void gas_spawn(TickCtx *c, int x, int y, const GasMix *g, float ts) {
    World *w = c->w;
    PR_ASSERT(in_reach(c, x, y));
    size_t i = world_index(w, x, y);
    Cell like = {ELEM_GAS, c->stamp, (uint8_t)rng_next(&c->rng), 0};
    w->cells[i] = like;
    w->gas[i] = *g;
    w->temp[i] = ts;
    ctx_arrive(c, x, y, like, ts);
}

static inline void gas_clear(World *w, size_t i) {
    w->cells[i].element = ELEM_VOID;
    w->cells[i].aux = 0;
    gas_mix_clear(&w->gas[i]);
    w->temp[i] = 0.0f;
}

// Mole fractions of a mixture holding n moles in total (n > 0).
static inline void gas_fractions(const GasMix *g, float n, float *x) {
    float inv = 1.0f / n;
    for (int s = 0; s < GAS_SPECIES; s++) {
        x[s] = g->m[s] * g_species_inv_molar[s] * inv;
    }
}

// Do two gas cells differ in composition (any species' mole fraction by GAS_MIX_TOL)?
static bool gas_mix_differs(const GasMix *a, float na, const GasMix *b, float nb) {
    if (na <= 0.0f || nb <= 0.0f) {
        return na != nb;
    }
    // |xa - xb| > tol, without divisions: |na_s * nb - nb_s * na| > tol * na * nb.
    float lim = GAS_MIX_TOL * na * nb;
    for (int s = 0; s < GAS_SPECIES; s++) {
        float d = (a->m[s] * nb - b->m[s] * na) * g_species_inv_molar[s];
        if (fabsf(d) > lim) {
            return true;
        }
    }
    return false;
}

// Mean spread (cells per tick into vacuum) of a mixture of molar mass mm (kg/mol): 3 for air,
// faster for lighter gas (sqrt of the molar mass ratio, like molecular speed), 1..6.
static inline int gas_spread_of(float mm) {
    float s = 3.0f * sqrtf(0.029f / PR_MAX(mm, 1e-3f));
    return PR_CLAMP((int)(s + 0.5f), 1, 6);
}

// Diffusion between cell i and its neighbour j: each species moves down its mole-fraction
// difference (no net moles: pressure is untouched), carrying its heat. Returns true if anything
// moved.
static bool gas_diffuse(TickCtx *c, size_t i, size_t j) {
    World *w = c->w;
    GasMix *a = gas_at(w, i), *b = gas_at(w, j);
    float na = gas_moles(a), nb = gas_moles(b);
    if (na <= 0.0f || nb <= 0.0f) {
        return false;
    }
    float xa[GAS_SPECIES], xb[GAS_SPECIES];
    gas_fractions(a, na, xa);
    gas_fractions(b, nb, xb);
    float big = 0.0f;
    for (int s = 0; s < GAS_SPECIES; s++) {
        big = PR_MAX(big, fabsf(xa[s] - xb[s]));
    }
    if (big <= GAS_MIX_TOL) {
        return false;
    }
    int wind = PR_MAX(gas_wind_speed(w->cells[i].aux), gas_wind_speed(w->cells[j].aux));
    float k = PR_MIN(GAS_DIFFUSE_K * (1.0f + GAS_TURB_PER_WIND * (float)wind), 0.5f);
    float n = 0.5f * (na + nb) * k;
    // Heat: what leaves a cell leaves at its temperature (energies E = C * T).
    float ta = w->temp[i], tb = w->temp[j];
    float ea = gas_heat_cap(a) * ta, eb = gas_heat_cap(b) * tb;
    for (int s = 0; s < GAS_SPECIES; s++) {
        float dm = n * (xa[s] - xb[s]) * g_species_molar[s]; // kg from a to b
        dm = PR_CLAMP(dm, -0.5f * b->m[s], 0.5f * a->m[s]);
        float q = dm * g_species_cp[s] * (dm > 0.0f ? ta : tb);
        a->m[s] -= dm;
        b->m[s] += dm;
        ea -= q;
        eb += q;
    }
    float ca = gas_heat_cap(a), cb = gas_heat_cap(b);
    w->temp[i] = ca > 0.0f ? ea / ca : ta;
    w->temp[j] = cb > 0.0f ? eb / cb : tb;
    return true;
}

// Does the cell at (x, y) have void beside it?
static inline bool gas_touches_void(const World *w, int x, int y) {
    for (int k = 0; k < 4; k++) {
        if (world_elem(w, x + k_dir4[k][0], y + k_dir4[k][1]) == ELEM_VOID) {
            return true;
        }
    }
    return false;
}

// Pressure equalisation: the cell trades gas with a gas cell so that both end at the same
// pressure. The partner is the last gas cell met walking a random 1..GAS_REACH cells from (x, y)
// in direction (dx, dy), if it has nearly the same temperature; otherwise the adjacent cell. The
// moved gas carries its composition and heat, which mix into the receiver: long jumps between
// cells at different temperatures would smear heat across the room and smother convection.
// Returns true if gas moved (*px, *py: the partner).
static bool gas_equalize(TickCtx *c, int x, int y, int dx, int dy, float pv, int *px, int *py) {
    World *w = c->w;
    size_t i = world_index(w, x, y);
    // Half the time the full reach (where a gentle gradient shows), else 1..GAS_REACH cells.
    int reach = rng_bit(&c->rng) ? GAS_REACH : 1 + (int)rng_range(&c->rng, GAS_REACH);
    // Cheap test first: the partner it would most likely pick (the cell `reach` away, else the
    // adjacent one) at the same pressure: nothing to do (still gas, the common case).
    int fx = x + dx * reach, fy = y + dy * reach;
    if (world_elem(w, fx, fy) == ELEM_GAS &&
        !gas_differs_at(pv, gas_pv(w, world_index(w, fx, fy)), reach) &&
        !gas_differs_at(pv, gas_pv(w, world_index(w, x + dx, y + dy)), 1)) {
        return false;
    }
    int qx = x, qy = y;
    for (int s = 1; s <= reach; s++) {
        if (world_elem(w, qx + dx, qy + dy) != ELEM_GAS) {
            break;
        }
        qx += dx;
        qy += dy;
    }
    if (qx == x && qy == y) {
        return false;
    }
    if (fabsf(w->temp[world_index(w, qx, qy)] - w->temp[i]) > GAS_JUMP_MAX_DT) {
        qx = x + dx;
        qy = y + dy;
    }
    PR_ASSERT(in_reach(c, qx, qy));
    size_t pi = world_index(w, qx, qy);
    float a = pv, b = gas_pv(w, pi);
    if (!gas_differs_at(a, b, abs(qx - x) + abs(qy - y))) {
        return false;
    }
    // Thin gas drifting in a vacuum (touching void) is not fed by gas more than GAS_FEED_RATIO
    // times denser: dense gas expands into a vacuum by splitting (else every drifting cell would
    // grow dense and split again, and gas would fill a vacuum as millions of cells at once).
    float na = gas_moles(&w->gas[i]), nb = gas_moles(&w->gas[pi]);
    if ((a > GAS_FEED_RATIO * b && nb < GAS_SPLIT_MOLES && gas_touches_void(w, qx, qy)) ||
        (b > GAS_FEED_RATIO * a && na < GAS_SPLIT_MOLES && gas_touches_void(w, x, y))) {
        return false;
    }
    // Equal pressure afterwards (nearly: the moved gas brings its temperature): the sender gives
    // (a - b) / 2 of its moles-times-temperature, at most half of its gas.
    if (a > b) {
        gas_move_part(w, i, pi, PR_MIN((a - b) / (2.0f * a), 0.5f));
    } else {
        gas_move_part(w, pi, i, PR_MIN((b - a) / (2.0f * b), 0.5f));
    }
    ctx_arrive(c, qx, qy, w->cells[pi], w->temp[pi]);
    ctx_arrive(c, x, y, w->cells[i], w->temp[i]);
    c->stats.gas_transfers++;
    *px = qx;
    *py = qy;
    return true;
}

// Buoyancy compares densities at 1 atm (World.rho, recomputed each tick; the cell's own is
// fresh). Not only with the cell above: a mixture's density varies smoothly (diffusion blurs every
// boundary), so a heavy layer on a light one differs little cell to cell, yet the columns' weights
// differ. The mean density of the gas in GAS_BUOY_SCALES windows of the cell's column, h rows
// above it against h rows from it down (each stopping at a cell that is not gas), gives a contrast
// (rho_above - rho_below) / (rho_above + rho_below) at each height h; bigger structures move
// faster (buoyant speed grows as the square root of their size), so the drive is the largest of
// contrast * sqrt(h). The cell rises with probability K_BUOY * drive: at h = 1 by trading places
// with the cell above; at a bigger h it is lifted to the top of the denser gas above it (up to h
// rows) and that column shifts down one row, so a heavy layer or blob sinks as a whole rather than
// one cell at a time.
#define GAS_BUOY_SCALES 4
static const int8_t k_buoy_h[GAS_BUOY_SCALES] = {1, 4, 8, 16};
// Heavy gas spreading sideways compares the weight of the GAS_SPREAD_H rows at and above the cell
// in its own column with that of a column 1, 4, 8 or 16 cells to the side (the hydrostatic
// pressure at its height; a gravity current is driven by the difference over its length): the
// drive is the largest contrast times sqrt(distance), as for rising. Moving, the row from the
// cell to that distance shifts one cell outward and the gas at the far end takes the cell's
// place (the light gas flowing back over the current), so a smooth pool edge still flows.
#define GAS_SPREAD_H 8
static const int8_t k_spread_d[GAS_BUOY_SCALES] = {1, 4, 8, 16};

static inline float rho_at(const World *w, int x, int y) {
    return w->rho[world_index(w, x, y)];
}

// The buoyant drive of the gas at (x, y) (density rs) and the height at which it applies; 0 if
// stable. dir -1: rising (the window ahead is above, and must be denser); dir +1: sinking (the
// window ahead is below, and must be lighter). The window behind includes the cell.
static float gas_buoyancy(const World *w, int x, int y, float rs, int dir, int *scale) {
    float sa = 0.0f, sb = rs, drive = 0.0f;
    int na = 0, nb = 1, h = 0;
    bool open_b = true;
    *scale = 0;
    for (int k = 0; k < GAS_BUOY_SCALES; k++) {
        for (; h < k_buoy_h[k]; h++) {
            int ya = y + dir * (1 + h);
            if (world_elem(w, x, ya) != ELEM_GAS) {
                return drive; // the column ahead ends: no taller window
            }
            sa += rho_at(w, x, ya);
            na++;
            if (h > 0 && open_b) {
                int yb = y - dir * h;
                if (world_elem(w, x, yb) == ELEM_GAS) {
                    sb += rho_at(w, x, yb);
                    nb++;
                } else {
                    open_b = false;
                }
            }
        }
        float ma = sa / (float)na, mb = sb / (float)nb;
        float c = (float)(-dir) * (ma - mb) / (ma + mb);
        if (c > GAS_BUOY_TOL) {
            float d = c * sqrtf((float)k_buoy_h[k]);
            if (d > drive) {
                drive = d;
                *scale = k_buoy_h[k];
            }
        }
    }
    return drive;
}

// Buoyant moves in a column. Rows of gas from (x, y + dir) on, in direction dir, clearly heavier
// (heavier) or clearly lighter (!heavier) than ref: their number if that layer ends within
// MAX_MOVE rows (on other gas or on something that is not gas), else -1 (it goes on).
static int gas_layer(const World *w, int x, int y, int dir, float ref, bool heavier) {
    for (int d = 1; d <= MAX_MOVE; d++) {
        int yy = y + dir * d;
        if (world_elem(w, x, yy) != ELEM_GAS) {
            return d - 1;
        }
        float r = rho_at(w, x, yy);
        if (heavier ? r <= ref * (1.0f + GAS_BUOY_TOL) : r >= ref * (1.0f - GAS_BUOY_TOL)) {
            return d - 1;
        }
    }
    return -1;
}

// Moves the gas at (x, y) `depth` rows in direction dir; the rows it passes shift back one.
static void gas_shift(TickCtx *c, int x, int y, int dir, int depth) {
    for (int k = 0; k < depth; k++) {
        swap_cells(c, x, y + dir * k, x, y + dir * (k + 1));
    }
}

// The gas at (x, y), of density rs, moves by buoyancy: up through heavier gas (dir -1) or down
// through lighter gas (dir +1); h is the window height that showed the drive (gas_buoyancy).
// - If the layer ahead ends within MAX_MOVE rows, the cell crosses it to its far side and the
//   layer shifts back one row: a warm blob rises as a whole (cold air on it sinks through it), a
//   heavy blob sinks as a whole, two layers turn over cleanly.
// - Nothing clearly denser (lighter) just ahead, a smooth gradient: one row.
// - The layer ahead is thick, but the cell's own layer is thin: the cell just ahead crosses the
//   cell's layer to its far side instead (the case above, seen from the other side), at this
//   cell's drive: a lone warm cell rises one row, a thin warm blob shifts up a row as a whole. The
//   cell does not dive into the thick layer (that peeled a warm blob into thin rising streaks).
// - Both layers thick (a deep warm layer under cold air): a finger h rows into the layer ahead,
//   which is how such layers overturn (plumes).
static void gas_buoyant_move(TickCtx *c, int x, int y, float rs, int dir, int h) {
    const World *w = c->w;
    bool rising = dir < 0;
    int across = gas_layer(w, x, y, dir, rs, rising);
    if (across >= 0) {
        gas_shift(c, x, y, dir, PR_MAX(across, 1));
        return;
    }
    int own = gas_layer(w, x, y + dir, -dir, rho_at(w, x, y + dir), !rising);
    if (own >= 0) {
        gas_shift(c, x, y + dir, -dir, PR_MAX(own, 1));
        return;
    }
    gas_shift(c, x, y, dir, PR_CLAMP(h, 1, MAX_MOVE));
}

// Mean density of the gas in column x, rows y - GAS_SPREAD_H + 1 .. y (stopping upward at a cell
// that is not gas); the cell at (x, y) counts as `self`.
static float gas_column_weight(const World *w, int x, int y, float self) {
    float s = self;
    int n = 1;
    for (int k = 1; k < GAS_SPREAD_H && world_elem(w, x, y - k) == ELEM_GAS; k++) {
        s += rho_at(w, x, y - k);
        n++;
    }
    return s / (float)n;
}

// Is the gas at (x, y), of density rs, resting: on something that is not a lighter gas?
static inline bool gas_rests(const World *w, int x, int y, float rs) {
    ElementId b = world_elem(w, x, y + 1);
    if (b != ELEM_GAS) {
        return b != ELEM_VOID;
    }
    return rho_at(w, x, y + 1) >= rs * (1.0f - GAS_BUOY_TOL);
}

// ---- Reactions (M5; in the cell, D-035). docs/SIMULATION.md -> Reactions ----

// The gas at (x, y) burns if it holds a reaction's fuel and oxidiser and it (or a neighbour) is
// hot enough: all of the fuel the oxidiser allows reacts at once, into the product, and the heat
// released (up to FLAME_MAX_K) heats the cell. Mass is conserved exactly. Returns true if it
// reacted.
static bool react(TickCtx *c, int x, int y) {
    World *w = c->w;
    size_t i = world_index(w, x, y);
    GasMix *g = gas_at(w, i);
    for (int r = 0; r < REACTION_COUNT; r++) {
        const Reaction *rx = &g_reactions[r];
        int sa = elem_species(rx->a), sb = elem_species(rx->b), sp = elem_species(rx->product);
        if (g->m[sa] <= 0.0f || g->m[sb] <= 0.0f) {
            continue;
        }
        bool hot = w->temp[i] >= rx->ignite_k;
        for (int k = 0; k < 4 && !hot; k++) {
            int qx = x + k_dir4[k][0], qy = y + k_dir4[k][1];
            ElementId n = world_elem(w, qx, qy);
            hot = n != ELEM_OUT && g_elem_cap[n] > 0.0f &&
                  elem_temp(n, w->temp[world_index(w, qx, qy)]) >= rx->ignite_k;
        }
        if (!hot) {
            continue;
        }
        float burn = PR_MIN(g->m[sa], g->m[sb] / rx->b_per_a); // kg of fuel
        float energy = gas_heat_cap(g) * w->temp[i] + burn * rx->heat_per_kg;
        g->m[sa] -= burn;
        g->m[sb] -= burn * rx->b_per_a;
        g->m[sp] += burn * (1.0f + rx->b_per_a);
        w->temp[i] = PR_MIN(energy / gas_heat_cap(g), FLAME_MAX_K);
        ctx_arrive(c, x, y, w->cells[i], w->temp[i]);
        ctx_wake(c, x - 1, y - 1, x + 1, y + 1, true, false);
        c->stats.reactions++;
        return true;
    }
    return false;
}

// ---- Burning wood (M6b, D-044). docs/SIMULATION.md -> Burning wood ----

// A flame draws air from the WOOD_AIR_CELLS nearest gas cells (within WOOD_AIR_RADIUS), a quarter
// of the oxygen of each at most per tick: a cell is a 5 cm cube of wood (75 g, which needs 90 g of
// oxygen, the air of 2400 cells) and the gas next to it alone could not feed it; a real flame
// draws air in from a whole volume.
#define WOOD_AIR_CELLS  256
#define WOOD_AIR_RADIUS 16
#define WOOD_AIR_FRAC   0.25f

// Flame contact: a burning cell also heats the unburnt wood beside it, WOOD_SPREAD times as fast
// as conduction does (heat is moved, not made). Embers do it too (a cell with no oxygen).
static void burning_contact(TickCtx *c, int x, int y) {
    World *w = c->w;
    size_t i = world_index(w, x, y);
    float tw = w->temp[i];
    for (int k = 0; k < 4; k++) {
        int nx = x + k_dir4[k][0], ny = y + k_dir4[k][1];
        if (world_elem(w, nx, ny) != ELEM_WOOD) {
            continue;
        }
        size_t j = world_index(w, nx, ny);
        float q = PR_MIN(WOOD_SPREAD * g_heat_coef[ELEM_BURNING_WOOD][ELEM_WOOD],
                         0.2f * g_elem_cap[ELEM_WOOD]) *
                  PR_MAX(tw - w->temp[j], 0.0f);
        w->temp[j] += q * g_elem_inv_cap[ELEM_WOOD];
        tw -= q * g_elem_inv_cap[ELEM_BURNING_WOOD];
        ctx_arrive(c, nx, ny, w->cells[j], w->temp[j]);
    }
    w->temp[i] = tw;
    ctx_arrive(c, x, y, w->cells[i], tw);
}

// One tick of a burning wood cell at (x, y). It takes oxygen from the gas cells beside it that
// hold at least WOOD_MIN_O2 of it by volume (at most the cell burns in WOOD_BURN_TICKS), burns that much wood (its `aux` counts the share gone: a whole cell
// at 255, when it disappears), puts the carbon dioxide and water vapour of that burn into the
// cells the oxygen came from (hot, at the wood's temperature) and the rest of the heat of
// combustion into the wood (up to WOOD_FLAME_K; more is dropped, as FLAME_MAX_K). Mass is
// conserved exactly in the gas; without oxygen it smoulders (it only cools, and goes out below
// WOOD_OUT_K: heat pass).
static void update_burning_wood(TickCtx *c, int x, int y) {
    World *w = c->w;
    size_t i = world_index(w, x, y);
    keep_alive(c, x, y); // burning: visit it again next tick
    burning_contact(c, x, y);
    const float m_cell = g_elem_mass[ELEM_WOOD] * WOOD_FUEL_SCALE; // the fuel in a cell, kg
    const float need = m_cell / (float)WOOD_BURN_TICKS * WOOD_O2_PER_KG; // kg of O2 a tick
    int32_t near[WOOD_AIR_CELLS];
    int n_near = world_gas_region(w, x, y, x, y, WOOD_AIR_RADIUS, WOOD_AIR_CELLS, near);
    // The air the flame draws on: it burns only if that air is at least WOOD_MIN_O2 oxygen by
    // volume (moles) on the whole, and takes the oxygen from all of it, each cell in proportion.
    float o2_mol = 0.0f, all_mol = 0.0f, o2_kg = 0.0f;
    for (int q = 0; q < n_near; q++) {
        const GasMix *g = &w->gas[near[q]];
        o2_mol += g->m[GAS_O2] * g_species_inv_molar[GAS_O2];
        o2_kg += g->m[GAS_O2];
        all_mol += gas_moles(g);
    }
    if (n_near == 0 || o2_mol < WOOD_MIN_O2 * all_mol) {
        return; // smoulders
    }
    float drawn = PR_MIN(need, WOOD_AIR_FRAC * o2_kg);
    float draw_share = drawn / o2_kg;
    int n = n_near;
    size_t ni[WOOD_AIR_CELLS];
    float draw[WOOD_AIR_CELLS];
    for (int q = 0; q < n_near; q++) {
        ni[q] = (size_t)near[q];
        draw[q] = w->gas[near[q]].m[GAS_O2] * draw_share;
    }
    float tw = w->temp[i];
    // The oxygen leaves the cells it was drawn from (at their temperatures)...
    for (int k = 0; k < n; k++) {
        size_t j = ni[k];
        GasMix *g = &w->gas[j];
        float energy = (gas_heat_cap(g) - draw[k] * g_species_cp[GAS_O2]) * w->temp[j];
        g->m[GAS_O2] -= draw[k];
        w->temp[j] = energy / gas_heat_cap(g);
    }
    // ... and the products, hot, enter the nearest gas cell (the one above the wood first): a
    // flame, which rises.
    float burnt_all = drawn / WOOD_O2_PER_KG;
    float co2 = burnt_all * WOOD_CO2_PER_KG, h2o = burnt_all * WOOD_H2O_PER_KG;
    float prod_cp = co2 * g_species_cp[GAS_CO2] + h2o * g_species_cp[GAS_H2O];
    // The heat of combustion: WOOD_FUEL_HEAT of it goes back into the wood (radiation and
    // conduction from the flame, which keeps it burning and lights the wood beside it); the rest
    // heats the products and the gas cell they enter (the flame, up to WOOD_FLAME_K; what does
    // not fit goes into the wood too). The hot cell rises and air flows in behind it.
    float heat = burnt_all * WOOD_LHV * WOOD_HEAT_SCALE;
    float to_gas = heat * (1.0f - WOOD_FUEL_HEAT);
    {
        size_t j = (size_t)near[0];
        GasMix *g = &w->gas[j];
        float cap_before = gas_heat_cap(g);
        g->m[GAS_CO2] += co2;
        g->m[GAS_H2O] += h2o;
        float cap = gas_heat_cap(g);
        // Energy: the cell's own, the products at ambient (they are made from wood and oxygen
        // at ambient), and the heat released.
        float energy = cap_before * w->temp[j] + prod_cp * T_ROOM + to_gas;
        float t_gas = PR_MIN(energy / cap, WOOD_FLAME_K);
        to_gas = PR_MAX(energy - t_gas * cap, 0.0f); // what the flame cannot hold
        heat = heat * WOOD_FUEL_HEAT + to_gas;
        w->temp[j] = t_gas;
        ctx_arrive(c, (int)(j % (size_t)w->width), (int)(j / (size_t)w->width), w->cells[j],
                   w->temp[j]);
    }
    tw += heat * g_elem_inv_cap[ELEM_BURNING_WOOD];
    w->temp[i] = tw;
    ctx_arrive(c, x, y, w->cells[i], w->temp[i]);
    // The share gone: a random whole number of steps (of 1/255 of a cell) with the right mean.
    float steps = burnt_all * (255.0f / m_cell);
    int inc = (int)steps;
    if (rng_unit(&c->rng) < steps - (float)inc) {
        inc++;
    }
    Cell *self = &w->cells[i];
    if ((int)self->aux + inc >= 255) {
        *self = (Cell){ELEM_VOID, c->stamp, self->shade, 0}; // burnt away
        w->temp[i] = 0.0f;
    } else {
        self->aux = (uint8_t)(self->aux + inc);
    }
    ctx_wake(c, x - 1, y - 1, x + 1, y + 1, true, false);
    c->stats.reactions++;
}

// ---- Condensation of water vapour (D-035) ----

// Dew point (K) for a vapour partial pressure p (Pa): Antoine's equation inverted.
static float dew_point(float p) {
    float lg = log10f(PR_MAX(p, 1.0f) / 133.322f);
    float c = lg < 8.07131f - 1730.63f / (233.426f + 99.0f) ? 1730.63f / (8.07131f - lg) - 233.426f
                                                            : 1810.94f / (8.14019f - lg) - 244.485f;
    return c + 273.15f;
}

// Water vapour at (x, y) beyond saturation condenses: as much as warms the cell back to the dew
// point (latent heat) and no more than the excess. The condensed water becomes a droplet (a water
// cell, the gas pushed into the gas around) with probability mass / mass of a water cell; else it
// is gone (mass is conserved on average). Saturation is taken at 1 atm total pressure (D-035), so
// a compressed cell of steam just boiled off does not condense back at once. Returns true if the
// cell became water.
static bool condense(TickCtx *c, int x, int y) {
    World *w = c->w;
    size_t i = world_index(w, x, y);
    GasMix *g = gas_at(w, i);
    float n = gas_moles(g);
    float nv = g->m[GAS_H2O] * g_species_inv_molar[GAS_H2O];
    float t = w->temp[i];
    float pv = nv / n * P_ATM; // the vapour's partial pressure at 1 atm total
    float psat = water_saturation_pressure(t);
    if (pv <= psat * (1.0f + GAS_EQ_TOL)) {
        return false;
    }
    // Vapour that may stay at this temperature: nv_max / (n_other + nv_max) = psat / P_ATM.
    float xs = PR_MIN(psat / P_ATM, 0.999f);
    float nv_max = xs / (1.0f - xs) * (n - nv);
    float excess = (nv - nv_max) * g_species_molar[GAS_H2O];
    // As much as brings the cell to the dew point: (C t + dm L) / (C - dm cp_v) = t_dew.
    float t_dew = dew_point(pv);
    float warm = gas_heat_cap(g) * (t_dew - t) / (VAPOUR_LATENT + g_species_cp[GAS_H2O] * t_dew);
    float dm = PR_CLAMP(PR_MIN(excess, warm), 0.0f, g->m[GAS_H2O]);
    if (dm <= 0.0f) {
        return false;
    }
    float energy = gas_heat_cap(g) * t + dm * VAPOUR_LATENT;
    g->m[GAS_H2O] -= dm;
    float cap = gas_heat_cap(g);
    w->temp[i] = cap > 0.0f ? energy / cap : t;
    ctx_arrive(c, x, y, w->cells[i], w->temp[i]);
    keep_alive(c, x, y);
    float m_water = g_elem_mass[ELEM_WATER];
    if (rng_unit(&c->rng) >= dm / m_water) {
        if (gas_total(g) <= 0.0f) {
            gas_clear(w, i);
            ctx_wake(c, x - 1, y - 1, x + 1, y + 1, true, false);
        }
        return false;
    }
    // A droplet: the cell's gas goes into the gas around.
    int32_t found[CONDENSE_CELLS];
    int nf = world_gas_region(w, x, y, x, y, CONDENSE_RADIUS, CONDENSE_CELLS, found);
    if (nf == 0 && gas_total(g) > 0.0f) {
        return false;
    }
    if (nf > 0) {
        world_gas_spread_add(w, found, nf, g, w->temp[i]);
        for (int k = 0; k < nf; k++) {
            ctx_arrive(c, found[k] % w->width, found[k] / w->width, w->cells[found[k]],
                       w->temp[found[k]]);
        }
    }
    float tw = w->temp[i];
    gas_mix_clear(g);
    w->cells[i] = (Cell){ELEM_WATER, c->stamp, (uint8_t)rng_next(&c->rng), 0};
    w->temp[i] = tw;
    ctx_arrive(c, x, y, w->cells[i], tw);
    ctx_wake(c, x - CONDENSE_RADIUS - 1, y - CONDENSE_RADIUS - 1, x + CONDENSE_RADIUS + 1,
             y + CONDENSE_RADIUS + 1, true, true);
    c->stats.reactions++;
    return true;
}

// ---- Gas currents: moving matter stirs the air. docs/SIMULATION.md -> Gas currents ----

// A current loses one speed step with this probability (x/65536) per tick (about a second from
// full speed), passes speed minus WIND_PASS_LOSS to the cell it pushes, spreads to a random
// neighbour (minus WIND_SPREAD_LOSS) with probability WIND_SPREAD_P16, and turns 45 degrees,
// losing WIND_TURN_LOSS, when blocked.
#define WIND_DECAY_P16   16384u
#define WIND_PASS_LOSS   2
#define WIND_SPREAD_LOSS 4
#define WIND_SPREAD_P16  32768u
#define WIND_TURN_LOSS   3

// Falling matter (liquid or powder) kicks the gas it passes aside and, as a stream, drags the
// gas beside it down: the current's speed is FALL_WIND_BASE plus its fall speed (1/16 cells per
// tick) / 4. A lone falling cell (a clod, a drop, a spent bullet: nothing falling just above or
// below it) only kicks the gas it displaces, at FALL_WIND_LONE: its wake is about its own size
// and fades within a fraction of a second (at 4 a current never spreads and dies in about 16
// ticks), where a stream keeps pulling air along.
#define FALL_WIND_BASE 6
#define FALL_WIND_LONE 4

// Liquid or powder that is falling (non-zero fall speed) at (x, y).
static inline bool falling_matter(const World *w, int x, int y) {
    if (!world_in_bounds(w, x, y)) {
        return false;
    }
    Cell t = w->cells[world_index(w, x, y)];
    uint8_t b = g_elem_behavior[t.element];
    return (b == BEHAVIOR_LIQUID || b == BEHAVIOR_POWDER) && (t.aux & 0x7F) != 0;
}

// The gas cell at (x, y), now holding gas that a cell falling at `speed` to (x, target) displaced.
static void stir_by_fall(TickCtx *c, int x, int y, int target, int speed) {
    World *w = c->w;
    int side = rng_bit(&c->rng) ? 1 : -1;
    if (!falling_matter(w, x, y - 1) && !falling_matter(w, x, target + 1)) {
        if (gas_push_wind(cell_ptr(w, x, y), gas_wind_dir_of(side, -1), FALL_WIND_LONE)) {
            ctx_wake(c, x - 1, y - 1, x + 1, y + 1, false, false);
        }
        return;
    }
    int s = FALL_WIND_BASE + speed / 4;
    bool changed = gas_push_wind(cell_ptr(w, x, y), gas_wind_dir_of(side, -1), s);
    for (int k = -1; k <= 1; k += 2) { // entrained beside the falling column
        if (world_in_bounds(w, x + k, y)) {
            changed |= gas_push_wind(cell_ptr(w, x + k, y), 4, s / 2);
        }
    }
    if (changed) {
        ctx_wake(c, x - 2, y - 2, x + 2, y + 2, false, false);
    }
}

// Moves the gas at (x, y) along its current, if it has one. Returns true if it moved.
static inline bool gas_same_as_around(const World *w, int x, int y, size_t i);

static bool gas_follow_wind(TickCtx *c, int x, int y) {
    World *w = c->w;
    Cell *cell = cell_ptr(w, x, y);
    int s = gas_wind_speed(cell->aux);
    if (s == 0) {
        return false;
    }
    // In gas exactly like the gas around it (still, settled air), a current changes nothing that
    // could be seen or measured: moving the cell trades identical contents, and the mixing and heat
    // exchange it speeds up have nothing to act on. It is not simulated there: it stops. (A
    // falling clod or a bullet crossing a clear sky would otherwise leave a churning trail
    // hundreds of cells long, for seconds.)
    if (gas_same_as_around(w, x, y, world_index(w, x, y))) {
        cell->aux = 0;
        return false;
    }
    int d = gas_wind_dir(cell->aux);
    int s2 = s - (rng_chance16(&c->rng, WIND_DECAY_P16) ? 1 : 0);
    keep_alive(c, x, y);
    // Viscosity: the current spreads to a neighbour.
    if (s2 > WIND_SPREAD_LOSS && rng_chance16(&c->rng, WIND_SPREAD_P16)) {
        int k = (int)rng_range(&c->rng, 4);
        int nx = x + k_dir4[k][0], ny = y + k_dir4[k][1];
        if (world_in_bounds(w, nx, ny) &&
            gas_push_wind(cell_ptr(w, nx, ny), d, s2 - WIND_SPREAD_LOSS)) {
            keep_alive(c, nx, ny);
        }
    }
    if (rng_chance16(&c->rng, (uint32_t)s * 2048u)) {
        int dx = k_wind_dir[d][0], dy = k_wind_dir[d][1];
        int tx = x + dx, ty = y + dy;
        ElementId t = world_elem(w, tx, ty);
        if ((t == ELEM_VOID || g_elem_behavior[t] == BEHAVIOR_GAS) &&
            (dx == 0 || dy == 0 || diagonal_open(w, x, y, dx, dy))) {
            gas_push_wind(cell_ptr(w, tx, ty), d, s2 - WIND_PASS_LOSS); // pushed along
            cell->aux = gas_wind(d, s2);
            swap_cells(c, x, y, tx, ty);
            return true;
        }
        // Blocked: turn 45 degrees either way.
        cell->aux = gas_wind((d + (rng_bit(&c->rng) ? 1 : 7)) & 7, s2 - WIND_TURN_LOSS);
        return false;
    }
    cell->aux = gas_wind(d, s2);
    return false;
}

// Could the gas at (x, y) (holding n moles; pv: moles times temperature; rs: density at 1 atm)
// still do something with its neighbours? 0 if not (it sleeps); else GAS_MOBILE, plus
// GAS_PRESSURE if its pressure differs from a neighbour's or a cell's GAS_REACH away (only then is
// pressure equalised).
#define GAS_MOBILE   1
#define GAS_PRESSURE 2

// Does a neighbour's density at 1 atm (rn; k: its k_dir4 direction) give the gas cell of density
// rs something to do? Beside it, any difference (heavy gas spreads under light). Above or below,
// only an unstable one: heavier gas above, lighter gas below. Lighter gas lying on heavier is at
// rest: that alone is no reason to stay awake (and a cell kept awake for it could be thrown off a
// warm blob's edge as a stray streak).
static inline bool gas_density_unstable(int k, float rs, float rn) {
    if (k == 0) {
        return rn > rs * (1.0f + GAS_BUOY_TOL); // above
    }
    if (k == 3) {
        return rn < rs * (1.0f - GAS_BUOY_TOL); // below
    }
    return fabsf(rn - rs) > GAS_BUOY_TOL * rs;
}

static int gas_is_mobile(const World *w, int x, int y, float n, float pv, float rs) {
    int flags = 0;
    size_t i = world_index(w, x, y);
    const GasMix *g = &w->gas[i];
    for (int k = 0; k < 4; k++) {
        int nx = x + k_dir4[k][0], ny = y + k_dir4[k][1];
        ElementId e = world_elem(w, nx, ny);
        if (e == ELEM_VOID) {
            return GAS_MOBILE | GAS_PRESSURE; // expand into it (split, or move as a particle)
        }
        if (e != ELEM_GAS) {
            continue;
        }
        size_t ni = world_index(w, nx, ny);
        const GasMix *h = &w->gas[ni];
        float nn = gas_moles(h);
        if (gas_differs(pv, nn * w->temp[ni]) || n + nn < GAS_SPLIT_MOLES) {
            return GAS_MOBILE | GAS_PRESSURE;
        }
        if (flags == 0 &&
            (gas_mix_differs(g, n, h, nn) || gas_density_unstable(k, rs, w->rho[ni]))) {
            flags = GAS_MOBILE; // diffuse; rise, sink or spread
        }
    }
    // A smooth pressure gradient differs little between neighbours: compare with the cells
    // GAS_REACH away (right and below; the cells on the other side look this way), which pressure
    // equalisation reaches.
    for (int k = 0; k < 2; k++) {
        int fx = x + (k == 0 ? GAS_REACH : 0), fy = y + (k == 0 ? 0 : GAS_REACH);
        if (world_elem(w, fx, fy) == ELEM_GAS &&
            gas_differs_at(pv, gas_pv(w, world_index(w, fx, fy)), GAS_REACH)) {
            return GAS_MOBILE | GAS_PRESSURE;
        }
    }
    return flags;
}

// Is the gas cell i exactly like the gas (or solid) around it: every gas neighbour holding the same
// mixture at the same temperature, no void? Then it has nothing to do (the common case: still air).
static inline bool gas_same_as_around(const World *w, int x, int y, size_t i) {
    const GasMix *g = &w->gas[i];
    float t = w->temp[i];
    for (int k = 0; k < 4; k++) {
        int nx = x + k_dir4[k][0], ny = y + k_dir4[k][1];
        ElementId e = world_elem(w, nx, ny);
        if (e == ELEM_GAS) {
            size_t ni = world_index(w, nx, ny);
            const GasMix *h = &w->gas[ni];
            bool same = w->temp[ni] == t;
            for (int s = 0; s < GAS_SPECIES; s++) {
                same &= h->m[s] == g->m[s];
            }
            if (!same) {
                return false;
            }
        } else if (g_elem_behavior[e] != BEHAVIOR_STATIC && g_elem_behavior[e] != BEHAVIOR_POWDER) {
            return false; // void (expand), liquid (a bubble rises)
        }
    }
    return true;
}

static void update_gas(TickCtx *c, int x, int y) {
    World *w = c->w;
    size_t i = world_index(w, x, y);
    Cell self = w->cells[i];
    if (self.aux == 0 && !c->may_react && w->gas[i].m[GAS_H2O] <= 0.0f &&
        gas_same_as_around(w, x, y, i)) {
        return; // still and like its neighbours: sleeps
    }

    // Currents first: moving air carries the gas along.
    if (self.aux != 0 && gas_follow_wind(c, x, y)) {
        return;
    }
    // 0. Burning, only where something is hot enough to ignite; condensing water vapour.
    if (c->may_react && react(c, x, y)) {
        return;
    }
    if (w->gas[i].m[GAS_H2O] > 0.0f && condense(c, x, y)) {
        return;
    }
    GasMix *g = gas_at(w, i);
    float n = gas_moles(g);
    if (n <= 0.0f) {
        gas_clear(w, i); // nothing left (it all burned or condensed)
        ctx_wake(c, x - 1, y - 1, x + 1, y + 1, true, false);
        return;
    }
    float t = w->temp[i];
    float rs = gas_total(g) / n * (P_ATM / GAS_R) / PR_MAX(t, 1.0f); // density at 1 atm
    ElementId up = world_elem(w, x, y - 1);

    // Nothing to do (the cell is like its neighbours, within tolerance): sleep. Checked first,
    // as most cells visited are (still air in an awake chunk).
    int mobile = g_elem_behavior[up] == BEHAVIOR_LIQUID ? GAS_MOBILE | GAS_PRESSURE
                                                        : gas_is_mobile(w, x, y, n, n * t, rs);
    if (mobile == 0) {
        return;
    }

    // 1. Buoyancy: rise through denser gas above (by composition or temperature, so warm air
    //    rises and CO2 sinks), or, as a bubble, through liquid.
    uint32_t p_up = 0;
    int scale = 1;
    if (up == ELEM_GAS && rho_at(w, x, y - 1) >= rs * (1.0f - GAS_BUOY_TOL)) {
        // (Clearly lighter gas just above: stable here, no need to look further.)
        float drive = gas_buoyancy(w, x, y, rs, -1, &scale);
        p_up = (uint32_t)PR_MIN((float)K_BUOY_16 * drive, 65535.0f);
    } else if (g_elem_behavior[up] == BEHAVIOR_LIQUID) {
        p_up = g_buoy_p[up][ELEM_GAS];
    }
    if (p_up > 0 && rng_chance16(&c->rng, p_up)) {
        if (up == ELEM_GAS) {
            gas_buoyant_move(c, x, y, rs, -1, scale);
        } else {
            swap_cells(c, x, y, x, y - 1); // a bubble in liquid
        }
        return;
    }
    // ... or sink into clearly lighter gas just below (the same drive, looking down).
    if (world_elem(w, x, y + 1) == ELEM_GAS && rho_at(w, x, y + 1) < rs * (1.0f - GAS_BUOY_TOL)) {
        int down_scale = 1;
        float drive = gas_buoyancy(w, x, y, rs, 1, &down_scale);
        if (drive > 0.0f &&
            rng_chance16(&c->rng, (uint32_t)PR_MIN((float)K_BUOY_16 * drive, 65535.0f))) {
            gas_buoyant_move(c, x, y, rs, 1, down_scale);
            return;
        }
    }
    // 1b. Heavy gas resting on something spreads sideways under lighter gas beside it, where its
    //     column weighs more (a gravity current): a pool of CO2 flattens out over the floor, and
    //     cold air flows under warm air. Cheap test first: is the gas beside, or 16 cells away,
    //     lighter at all?
    int side = rng_bit(&c->rng) ? 1 : -1;
    if (world_elem(w, x + side, y) == ELEM_GAS && gas_rests(w, x, y, rs) &&
        rs > rho_at(w, x + side, y) * (1.0f + GAS_BUOY_TOL)) {
        float own = gas_column_weight(w, x, y, rs), drive = 0.0f, contrast[GAS_BUOY_SCALES];
        float best = 0.0f;
        int scales = 0;
        for (int k = 0; k < GAS_BUOY_SCALES; k++) {
            int d = k_spread_d[k];
            bool open = true;
            for (int j = k == 0 ? 1 : k_spread_d[k - 1] + 1; j <= d && open; j++) {
                open = world_elem(w, x + side * j, y) == ELEM_GAS;
            }
            if (!open) {
                break; // a wall or liquid in the row
            }
            int sx = x + side * d;
            if (k == 1 && world_elem(w, x + side * GAS_REACH, y) == ELEM_GAS &&
                rho_at(w, x + side * GAS_REACH, y) >= rs * (1.0f - GAS_BUOY_TOL)) {
                break; // nothing lighter further off: a local step only
            }
            float other = gas_column_weight(w, sx, y, rho_at(w, sx, y));
            contrast[k] = (own - other) / (own + other);
            best = PR_MAX(best, contrast[k]);
            drive = PR_MAX(drive, contrast[k] * sqrtf((float)d));
            scales = k + 1;
        }
        // The row shifts over the shortest distance that shows most of the contrast: beside a
        // sharp boundary (a hot plume) the cell next to it, at a smooth pool edge further.
        int reach = 1;
        for (int k = 0; k < scales; k++) {
            if (contrast[k] >= 0.5f * best) {
                reach = k_spread_d[k];
                break;
            }
        }
        if (drive > GAS_BUOY_TOL) {
            p_up |= 1; // stay awake
            if (rng_chance16(&c->rng, (uint32_t)PR_MIN((float)K_BUOY_16 * drive, 65535.0f))) {
                for (int j = reach; j > 0; j--) { // the far cell comes back, the row shifts out
                    swap_cells(c, x + side * j, y, x + side * (j - 1), y);
                }
                return;
            }
        }
    }

    // 2. One random direction per tick.
    int r = (int)rng_range(&c->rng, 4);
    int dx = k_dir4[r][0], dy = k_dir4[r][1];
    int nx = x + dx, ny = y + dy;
    ElementId ne = world_elem(w, nx, ny);
    if (ne == ELEM_VOID) {
        // 2a. Expansion into vacuum. Dense gas splits: half of it goes 1..spread cells into the
        //     void, so the gas fills the space and thins out. Thin gas moves as a whole.
        bool dense = n >= GAS_SPLIT_MOLES;
        int spread = gas_spread_of(gas_total(g) / n);
        int steps = dense ? 1 + (int)rng_range(&c->rng, (uint32_t)spread) : spread;
        int tx = x, ty = y;
        while (steps-- > 0 && world_elem(w, tx + dx, ty + dy) == ELEM_VOID) {
            tx += dx;
            ty += dy;
        }
        if (dense) {
            GasMix half = *g;
            gas_mix_scale(&half, 0.5f);
            gas_mix_add(g, &half, -1.0f);
            gas_spawn(c, tx, ty, &half, t);
            gas_wake(c, x, y, tx, ty);
            c->stats.cells_moved++;
        } else {
            swap_cells(c, x, y, tx, ty);
        }
        return;
    }
    if (ne == ELEM_GAS) {
        size_t ni = world_index(w, nx, ny);
        // 2b. Two thin cells merge (thin gas stays a few particles rather than many ever-thinner
        //     ones).
        if (n + gas_moles(&w->gas[ni]) < GAS_SPLIT_MOLES) {
            gas_move_part(w, i, ni, 1.0f);
            ctx_arrive(c, nx, ny, w->cells[ni], w->temp[ni]);
            gas_clear(w, i);
            gas_wake(c, x, y, nx, ny);
            c->stats.cells_moved++;
            return;
        }
        // 2c. Diffusion with that neighbour, and pressure with a cell up to GAS_REACH away.
        bool diffused = gas_diffuse(c, i, ni);
        int px = nx, py = ny;
        bool equalized =
            (mobile & GAS_PRESSURE) && gas_equalize(c, x, y, dx, dy, gas_pv(w, i), &px, &py);
        if (diffused || equalized) {
            if (diffused) {
                ctx_arrive(c, nx, ny, w->cells[ni], w->temp[ni]);
                ctx_arrive(c, x, y, w->cells[i], w->temp[i]);
                gas_wake(c, x, y, nx, ny);
            }
            if (equalized) {
                gas_wake(c, x, y, px, py);
            }
            return;
        }
    }

    // 3. Did not move, but something is still possible (checked above): stay awake.
    keep_alive(c, x, y);
}

// ---- Powders (M5): sand, loose soil, rubble. docs/SIMULATION.md -> Powders ----

// In a liquid a powder sinks one cell with this probability (x/65536) per tick (about 0.15 m/s;
// grains settle slowly in water) instead of falling freely.
#define POWDER_SINK_P16 3000u

// A powder falls (accelerating like a liquid in void or gas), sinks slowly through liquids,
// slides diagonally down when blocked (which piles it at about 45 degrees), and sleeps at rest.
static void update_powder(TickCtx *c, int x, int y) {
    World *w = c->w;
    Cell *cell = cell_ptr(w, x, y);
    uint32_t d = g_elem_density_q[cell->element];
    int speed = cell->aux & 0x7F;

    if (can_displace(w, x, y + 1, d)) {
        if (g_elem_behavior[world_elem(w, x, y + 1)] == BEHAVIOR_LIQUID) {
            // Sinking through liquid: slowly, no acceleration.
            cell->aux = 1;
            if (rng_chance16(&c->rng, POWDER_SINK_P16)) {
                swap_cells(c, x, y, x, y + 1);
                cell_ptr(w, x, y + 1)->aux = 1;
            } else {
                keep_alive(c, x, y);
            }
            return;
        }
        if (speed < FALL_SPEED_MAX && rng_chance16(&c->rng, GRAVITY_INC_P16)) {
            speed++;
        }
        int steps = 1 + (speed >> 4), target = y;
        for (int ny = y + 1; ny <= y + MAX_MOVE && steps > 0; ny++) {
            ElementId t = world_elem(w, x, ny);
            if (!can_displace(w, x, ny, d) || g_elem_behavior[t] == BEHAVIOR_LIQUID) {
                break; // stop at liquid: sink from there
            }
            target = ny;
            steps--;
        }
        speed = PR_MAX(speed, 1);
        swap_cells(c, x, y, x, target);
        cell_ptr(w, x, target)->aux = (uint8_t)speed;
        if (g_elem_behavior[world_elem(w, x, y)] == BEHAVIOR_GAS) {
            stir_by_fall(c, x, y, target, speed);
        }
        return;
    }
    cell->aux = 0; // landed
    int first = rng_bit(&c->rng) ? 1 : -1;
    for (int k = 0; k < 2; k++) {
        int dx = k == 0 ? first : -first;
        if (can_displace(w, x + dx, y + 1, d) && can_displace(w, x + dx, y, d) &&
            diagonal_open(w, x, y, dx, 1)) {
            swap_cells(c, x, y, x + dx, y + 1);
            return;
        }
    }
    // At rest: sleeps (no wake).
}

static void update_chunk(TickCtx *c) {
    World *w = c->w;
    DirtyRect r = world_chunk(w, c->cx, c->cy)->cur;

    c->rng = rng_from_seed(w->seed ^ pr_splitmix64(w->tick) ^
                           (((uint64_t)(uint32_t)c->cx << 32) | (uint32_t)c->cy));
    c->stats.chunks_active++;
    // Reactions are possible only if this chunk or one around it holds something hot enough to
    // ignite (each chunk keeps the range of its temperatures).
    c->may_react = false;
    for (int ny = c->cy - 1; ny <= c->cy + 1; ny++) {
        for (int nx = c->cx - 1; nx <= c->cx + 1; nx++) {
            if (nx >= 0 && ny >= 0 && nx < w->chunks_x && ny < w->chunks_y &&
                world_chunk(w, nx, ny)->temp_hi >= g_min_ignite_k) {
                c->may_react = true;
            }
        }
    }

    // Bottom to top; horizontal direction alternates every tick (no left/right bias). Only the
    // cells of the rect inside its woken tiles (up to 2 spans per tile row).
    bool left_to_right = (w->tick & 1) == 0;
    for (int ty = r.max_y >> TILE_SHIFT; ty >= r.min_y >> TILE_SHIFT; ty--) {
        int sx0[2], sx1[2];
        int ns = dirty_rect_spans(&r, ty, sx0, sx1);
        if (ns == 0) {
            continue;
        }
        int ly_top = PR_MAX(r.min_y, ty * TILE_SIZE);
        int ly_bottom = PR_MIN(r.max_y, ty * TILE_SIZE + TILE_SIZE - 1);
        for (int ly = ly_bottom; ly >= ly_top; ly--) {
            int y = c->base_y + ly;
            Cell *row = &w->cells[world_index(w, 0, y)];
            for (int s = 0; s < ns; s++) {
                int k = left_to_right ? s : ns - 1 - s;
                int n = sx1[k] - sx0[k];
                for (int i = 0; i <= n; i++) {
                    int x = c->base_x + (left_to_right ? sx0[k] + i : sx1[k] - i);
                    Cell *cell = &row[x];
                    uint8_t b = g_elem_behavior[cell->element];
                    if (b <= BEHAVIOR_STATIC) {
                        if (PR_UNLIKELY(cell->element == ELEM_BURNING_WOOD) &&
                            cell->stamp != c->stamp) {
                            cell->stamp = c->stamp;
                            update_burning_wood(c, x, y);
                        }
                        continue;
                    }
                    if (cell->stamp == c->stamp) {
                        continue;
                    }
                    cell->stamp = c->stamp;
                    c->stats.cells_visited++;
                    switch (b) {
                        case BEHAVIOR_LIQUID:
                            update_liquid(c, x, y);
                            break;
                        case BEHAVIOR_GAS:
                            update_gas(c, x, y);
                            break;
                        case BEHAVIOR_POWDER:
                            update_powder(c, x, y);
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
}

// Scratch memory for one phase: the chunks to update and one TickCtx per chunk.
typedef struct PhaseScratch {
    int *chunk_list; // the awake chunks (gas_rho_update)
    TickCtx *ctx;
    int *order; // the phase's jobs, costliest first
    int capacity;
} PhaseScratch;

// A phase's jobs, run in `order`. The order only changes which thread runs what, and when: jobs
// of one phase never touch the same cells, and their results are merged in `ctx` order.
typedef struct PhaseRun {
    TickCtx *ctx;
    const int *order;
} PhaseRun;

static void phase_job(void *user, int index) {
    // Work on a copy on this thread's stack: jobs' contexts sit side by side in one array, and
    // updating them in place (every move touches the stats and wakes) made threads fight over
    // shared cache lines.
    const PhaseRun *run = user;
    TickCtx *slot = &run->ctx[run->order[index]];
    TickCtx c = *slot;
    update_chunk(&c);
    *slot = c;
}

// Orders a phase's jobs costliest first: a counting sort on the chunks' `cost` (at most 4096, one
// visit per cell) in buckets of 32 cells. Stable, so ties keep the grid order.
#define COST_BUCKETS ((CHUNK_SIZE * CHUNK_SIZE >> 5) + 1)
static void order_jobs(const World *w, const TickCtx *ctx, int jobs, int *order) {
    int start[COST_BUCKETS + 1] = {0};
    for (int j = 0; j < jobs; j++) {
        int b = PR_MIN(w->chunks[ctx[j].cy * w->chunks_x + ctx[j].cx].cost >> 5, COST_BUCKETS - 1);
        start[COST_BUCKETS - 1 - b + 1]++; // bucket 0 holds the costliest
    }
    for (int b = 0; b < COST_BUCKETS; b++) {
        start[b + 1] += start[b];
    }
    for (int j = 0; j < jobs; j++) {
        int b = PR_MIN(w->chunks[ctx[j].cy * w->chunks_x + ctx[j].cx].cost >> 5, COST_BUCKETS - 1);
        order[start[COST_BUCKETS - 1 - b]++] = j;
    }
}

static PhaseScratch *get_scratch(World *w) {
    int capacity = ((w->chunks_x + 1) / 2) * ((w->chunks_y + 1) / 2);
    if (!w->sim_scratch) {
        size_t bytes = sizeof(PhaseScratch) + (size_t)capacity * sizeof(TickCtx) +
                       (size_t)capacity * sizeof(int) +
                       (size_t)w->chunks_x * (size_t)w->chunks_y * sizeof(int);
        PhaseScratch *s = pr_aligned_alloc_zero(64, bytes + 64);
        PR_ASSERT_ALWAYS(s != NULL);
        s->capacity = capacity;
        s->ctx = (TickCtx *)(((uintptr_t)(s + 1) + 63) & ~(uintptr_t)63);
        s->order = (int *)(s->ctx + capacity);
        s->chunk_list = s->order + capacity;
        w->sim_scratch = s;
    }
    return w->sim_scratch;
}

// Gas densities (World.rho) of the cells awake this tick (each chunk's rect inside its woken
// tiles), recomputed in parallel before the movement pass (each job writes only its own chunk). A
// cell that changes wakes itself for the next tick, so they are at most a tick old.
typedef struct GasRhoJob {
    World *w;
    const int *chunks;
} GasRhoJob;

static void gas_rho_job(void *user, int index) {
    const GasRhoJob *job = user;
    World *w = job->w;
    int ci = job->chunks[index];
    const DirtyRect *r = &w->chunks[ci].cur;
    int x0 = (ci % w->chunks_x) << CHUNK_SHIFT, y0 = (ci / w->chunks_x) << CHUNK_SHIFT;
    for (int ty = r->min_y >> TILE_SHIFT; ty <= r->max_y >> TILE_SHIFT; ty++) {
        int sx0[2], sx1[2];
        int ns = dirty_rect_spans(r, ty, sx0, sx1);
        int ly0 = PR_MAX(r->min_y, ty * TILE_SIZE);
        int ly1 = PR_MIN(r->max_y, ty * TILE_SIZE + TILE_SIZE - 1);
        for (int ly = ly0; ly <= ly1; ly++) {
            size_t row = world_index(w, x0, y0 + ly);
            for (int s = 0; s < ns; s++) {
                for (int lx = sx0[s]; lx <= sx1[s]; lx++) {
                    size_t i = row + (size_t)lx;
                    w->rho[i] = w->cells[i].element == ELEM_GAS
                                    ? gas_mix_density_atm(&w->gas[i], w->temp[i])
                                    : 0.0f;
                }
            }
        }
    }
}

static void gas_rho_update(World *w, int *list) {
    int count = 0;
    for (int i = 0; i < w->chunks_x * w->chunks_y; i++) {
        if (!dirty_rect_empty(&w->chunks[i].cur)) {
            list[count++] = i;
        }
    }
    GasRhoJob job = {w, list};
    jobs_run(w->jobs, count, gas_rho_job, &job);
}

void sim_tick(World *w, SimStats *stats) {
    SimStats total = {0};
    PhaseScratch *scratch = get_scratch(w);
    uint8_t stamp = (uint8_t)w->tick;

    // Creatures first (serial, in list order); what they move is woken for this tick.
    creatures_update(w);
    pickups_update(w);
    rigids_update(w);

    int n = w->chunks_x * w->chunks_y;
    for (int i = 0; i < n; i++) {
        Chunk *ch = &w->chunks[i];
        if (!ch->active) { // frozen: what woke it waits (next, head_dirty_next, heat_active_next)
            dirty_rect_clear(&ch->cur);
            ch->head_dirty = 0;
            ch->heat_active = 0;
            continue;
        }
        ch->cur = ch->next;
        dirty_rect_clear(&ch->next);
        ch->head_dirty = ch->head_dirty_next;
        ch->head_dirty_next = 0;
        ch->heat_active = ch->heat_active_next;
        ch->heat_active_next = 0;
    }
    // Cells moved into these chunks last tick: they need the heat pass only if temperatures differ
    // within the chunk or against its neighbours (a world at one temperature costs nothing).
    for (int cy = 0; cy < w->chunks_y; cy++) {
        for (int cx = 0; cx < w->chunks_x; cx++) {
            Chunk *ch = world_chunk(w, cx, cy);
            if (!ch->heat_check || !ch->active) {
                continue;
            }
            ch->heat_check = 0;
            float lo = ch->temp_lo, hi = ch->temp_hi;
            static const int8_t d4[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (int k = 0; k < 4; k++) {
                int nx = cx + d4[k][0], ny = cy + d4[k][1];
                if (nx >= 0 && ny >= 0 && nx < w->chunks_x && ny < w->chunks_y) {
                    Chunk *nb = world_chunk(w, nx, ny);
                    lo = PR_MIN(lo, nb->temp_lo);
                    hi = PR_MAX(hi, nb->temp_hi);
                }
            }
            if (hi - lo > HEAT_UNIFORM_K) {
                ch->heat_active = 1;
            }
        }
    }

    // Water pressure: relabel still liquid where it changed and rebuild the bodies. This may add
    // whole chunks to this tick's dirty rects (where a body's top rose).
    // Heat first: conduction and phase changes (cells that melt, freeze, boil or condense must be
    // relabelled for water pressure and simulated this tick).
    HeatStats heat;
    heat_update(w, &heat);
    total.heat_chunks = heat.chunks;
    total.phase_changes = heat.phase_changes;

    pressure_update(w);
    gas_rho_update(w, scratch->chunk_list);

    // Splashes only while the particle list has plenty of room.
    bool splash_ok = w->particle_count < SPLASH_LIST_LIMIT;

    // 4-phase checkerboard. Chunks of one phase are two chunks apart, so their jobs (each
    // reaching MAX_MOVE = half a chunk beyond its chunk) never touch the same cells. The phase
    // that goes first rotates every tick, so no group of chunks always moves first.
    for (int i = 0; i < 4; i++) {
        int phase = (int)((i + w->tick) & 3);
        int px = phase & 1, py = phase >> 1;
        int jobs = 0;
        for (int cy = py; cy < w->chunks_y; cy += 2) {
            for (int cx = px; cx < w->chunks_x; cx += 2) {
                if (dirty_rect_empty(&world_chunk(w, cx, cy)->cur)) {
                    continue;
                }
                TickCtx *c = &scratch->ctx[jobs++];
                memset(c, 0, sizeof(*c));
                c->w = w;
                c->splash_ok = splash_ok;
                c->stamp = stamp;
                c->cx = cx;
                c->cy = cy;
                c->base_x = cx << CHUNK_SHIFT;
                c->base_y = cy << CHUNK_SHIFT;
                for (int k = 0; k < 9; k++) {
                    dirty_rect_clear(&c->wake[k]);
                    c->arr_lo[k] = 1e30f;
                    c->arr_hi[k] = -1e30f;
                }
            }
        }
        PR_ASSERT(jobs <= scratch->capacity);

        // Costliest chunks first (by what they visited last time), so that a heavy chunk does not
        // start last and leave the other threads idle while it finishes. A phase lasts at least
        // as long as its costliest job.
        order_jobs(w, scratch->ctx, jobs, scratch->order);
        PhaseRun run = {scratch->ctx, scratch->order};
        jobs_run(w->jobs, jobs, phase_job, &run);

        // Merge each job's wakes into the chunks. Min/max and OR are order-independent, so the
        // result does not depend on which thread ran which job.
        for (int j = 0; j < jobs; j++) {
            TickCtx *c = &scratch->ctx[j];
            for (int k = 0; k < 9; k++) {
                int ccx = c->cx + k % 3 - 1, ccy = c->cy + k / 3 - 1;
                if (ccx < 0 || ccy < 0 || ccx >= w->chunks_x || ccy >= w->chunks_y) {
                    continue;
                }
                Chunk *chunk = world_chunk(w, ccx, ccy);
                dirty_rect_merge(&chunk->next, &c->wake[k]);
                if (k == 4) {
                    chunk->cost = (uint16_t)PR_MIN(c->stats.cells_visited, 65535u);
                }
                chunk->render_dirty |= c->render[k];
                chunk->head_dirty_next |= c->liquid[k];
                if (c->arr_lo[k] <= c->arr_hi[k]) { // cells moved in, with their temperatures
                    chunk->temp_lo = PR_MIN(chunk->temp_lo, c->arr_lo[k]);
                    chunk->temp_hi = PR_MAX(chunk->temp_hi, c->arr_hi[k]);
                    chunk->heat_check = 1;
                }
            }
            total.cells_visited += c->stats.cells_visited;
            total.cells_moved += c->stats.cells_moved;
            total.gas_transfers += c->stats.gas_transfers;
            total.reactions += c->stats.reactions;
            total.chunks_active += c->stats.chunks_active;
        }
        // Water-pressure transfers asked for in this phase, serially, in job order.
        for (int j = 0; j < jobs; j++) {
            TickCtx *c = &scratch->ctx[j];
            total.requests += (uint32_t)c->nreq;
            total.transfers += (uint32_t)pressure_apply(w, c->req, c->nreq);
        }
        // Particles launched in this phase (splashes), in job order. If the list is full, the
        // cell goes straight back onto the grid.
        for (int j = 0; j < jobs; j++) {
            TickCtx *c = &scratch->ctx[j];
            for (int k = 0; k < c->nspawn; k++) {
                if (!particle_add(w, &c->spawn[k])) {
                    particle_land_now(w, &c->spawn[k]);
                }
            }
            total.launched += (uint32_t)c->nspawn;
        }
    }

    // Free particles fly, and land back into the grid (serial, in list order).
    total.launched += w->launched_by_pressure;
    w->launched_by_pressure = 0;
    particles_update(w);
    total.particles = (uint32_t)w->particle_count;

    w->tick++;
    if (stats) {
        *stats = total;
    }
}

int sim_pressure_depth(const World *w, int x, int y) {
    if (!world_in_bounds(w, x, y) || !w->piece[world_index(w, x, y)]) {
        return -1;
    }
    int32_t row = pressure_body_top_row(w, x, y);
    return row == PRESSURE_NO_TOP ? -1 : (int)PR_MAX(y - row, 0);
}

float sim_liquid_pressure(const World *w, int x, int y) {
    if (!world_in_bounds(w, x, y) || !w->piece[world_index(w, x, y)]) {
        return -1.0f;
    }
    int32_t top = pressure_body_top(w, x, y);
    if (top == PRESSURE_NO_TOP) {
        return -1.0f;
    }
    ElementId e = w->cells[world_index(w, x, y)].element;
    return (float)PR_MAX(y - top, 0) * g_elem_defs[e].density * GRAVITY * CELL_SIZE_M;
}

void sim_run_ticks(World *w, int n) {
    for (int i = 0; i < n; i++) {
        sim_tick(w, NULL);
    }
}
