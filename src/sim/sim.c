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
#include "sim/pressure.h"

#include <math.h>
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
static void stir_by_fall(TickCtx *c, int x, int y, int speed);

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
    float m = w->mass[i0];
    w->mass[i0] = w->mass[i1];
    w->mass[i1] = m;
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
            stir_by_fall(c, x, y, speed);
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
        w->mass[i] = 0.0f;
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

// Bulk buoyancy between different gases (docs/SIMULATION.md -> Gases). A cell of a gas mixed
// into another stands for gas mixed at the molecular scale: it does not sink or rise on its own.
// Buoyancy moves bulk gas: the mean buoyancy density of the gas cells in a window (each gas at its
// table density at 0 degC; other cells not counted) is compared with the window next to it, and
// gas moves only where the difference is significant (bulk_excess), so the noise of a mixture
// does not move it.
#define BULK_HALF_W       3 // windows are 7 cells wide, and 4, 8 or BULK_TALL rows tall
#define BULK_TALL         16
#define BULK_Z            2.0f  // significance: standard errors of the difference of the means
#define BULK_MIN_CONTRAST 0.01f // and least (rho_a - rho_b) / (rho_a + rho_b)
#define BULK_K            4.0f  // a parcel sinks with probability BULK_K * contrast per tick
#define SIDE_NOISE        1.5f  // against the lighter of two side windows: BULK_Z times this
#define PARCEL_MAX        31    // rows a light cell is lifted through a heavy parcel (MAX_MOVE)
#define SPREAD_W          6     // spreading sideways (gas_spread_p): windows 6 wide,
#define SPREAD_H          10    // 10 tall,
#define SPREAD_Z          2.5f  // and stricter, as moving sideways never settles a mixture
#define SPREAD_EVERY      8     // tried one tick in this many (at this times the probability)

// Bulk buoyancy is tried one tick in BULK_EVERY per cell (at BULK_EVERY times the probability).
// Every BULK_FULL_EVERY ticks, all cells try it with all window heights; only then can a cell
// with a denser gas above or beside it go to sleep (all cells of a chunk on the same tick, so a
// settled mixture sleeps; chunks take turns, which spreads the cost over the ticks).
#define BULK_EVERY      2
#define BULK_FULL_EVERY 16

// Sum and sum of squares of the buoyancy densities of the gas cells in a window, and their count.
typedef struct BulkSum {
    float s, q;
    int n;
} BulkSum;

// Adds the gas cells in columns x0..x1 of row y.
static inline void bulk_row(const World *w, int x0, int x1, int y, BulkSum *b) {
    if (y < 0 || y >= w->height) {
        return;
    }
    const Cell *row = &w->cells[world_index(w, 0, y)];
    float sum = 0.0f, sq = 0.0f;
    int n = 0;
    for (int x = PR_MAX(x0, 0); x <= PR_MIN(x1, w->width - 1); x++) {
        float d = g_gas_buoy_density[row[x].element]; // 0 if not a gas: branch-free
        sum += d;
        sq += d * d;
        n += d > 0.0f;
    }
    b->s += sum;
    b->q += sq;
    b->n += n;
}

static BulkSum bulk_window(const World *w, int x0, int x1, int y0, int y1) {
    BulkSum b = {0.0f, 0.0f, 0};
    for (int y = y0; y <= y1; y++) {
        bulk_row(w, x0, x1, y, &b);
    }
    return b;
}

static inline float bulk_mean(const BulkSum *b) {
    return b->n ? b->s / (float)b->n : 0.0f;
}

static inline float bulk_row_mean(const World *w, int x0, int x1, int y) {
    BulkSum b = {0.0f, 0.0f, 0};
    bulk_row(w, x0, x1, y, &b);
    return bulk_mean(&b);
}

// Is the gas in window a denser than in window b, by z standard errors of the difference of the
// means and by BULK_MIN_CONTRAST? Returns the contrast (rho_a - rho_b) / (rho_a + rho_b), or 0.
// The standard error is the one if both windows held the same mixture (the pooled spread of their
// densities): the noise that cells of a mixture make, which must not move it.
static float bulk_excess(const BulkSum *a, const BulkSum *b, float z) {
    if (a->n == 0 || b->n == 0) {
        return 0.0f;
    }
    float ma = a->s / (float)a->n, mb = b->s / (float)b->n, d = ma - mb;
    if (d <= 0.0f) {
        return 0.0f;
    }
    // Spread of the densities if both windows held the same mixture (pooled).
    float n = (float)(a->n + b->n), m = (a->s + b->s) / n;
    float v = PR_MAX((a->q + b->q) / n - m * m, 0.0f);
    if (d * d < z * z * v * (1.0f / (float)a->n + 1.0f / (float)b->n)) {
        return 0.0f;
    }
    float contrast = d / (ma + mb);
    return contrast >= BULK_MIN_CONTRAST ? contrast : 0.0f;
}

// Gases of different buoyancy densities traded places in columns x0..x1, rows y0..y1: wake every
// cell whose bulk windows see them. (Oxygen and nitrogen share one density: air needs no wake.)
static inline void gas_wake_bulk(TickCtx *c, int x0, int y0, int x1, int y1) {
    ctx_wake(c, x0 - 3 * BULK_HALF_W - 2, y0 - BULK_TALL, x1 + 3 * BULK_HALF_W + 2,
             y1 + BULK_TALL + 1, false, false);
}

// Is the gas at (x, y) a bubble in heavy gas: the 3 cells above it are denser gases, and so are
// all but at most 2 of the gas cells in the 7 x 3 cells above it? (Light gas left inside a heavy
// parcel by gas_lift, when the parcel is taller than PARCEL_MAX. Not so in a mixture.)
static inline bool gas_is_bubble(const World *w, int x, int y, ElementId self) {
    int light = 0, gas = 0;
    for (int dy = -1; dy >= -3; dy--) { // the row above first: most cells fail there
        for (int dx = -BULK_HALF_W; dx <= BULK_HALF_W; dx++) {
            ElementId n = world_elem(w, x + dx, y + dy);
            if (g_elem_behavior[n] != BEHAVIOR_GAS) {
                if (dy == -1 && dx >= -1 && dx <= 1) {
                    return false;
                }
                continue;
            }
            gas++;
            if (g_buoy_p[n][self] == 0) {
                if ((dy == -1 && dx >= -1 && dx <= 1) || ++light > 2) {
                    return false;
                }
            }
        }
    }
    return gas >= 15;
}

// Probability (x/65536) that the fluid at (x, y) rises through the fluid directly above it:
// K_BUOY * (rho_above - rho_self) / (rho_above + rho_self), with densities at the cells' physical
// temperatures (so hot gas rises through the same gas when it is cold). 0 if it is not lighter.
// Gas densities are taken at 1 atm (elem_fluid_density): pressure does not make a gas sink.
// Between two different gases: a warmer cell rises (a thermal: K_BUOY * (Ts - Tu) / (Ts + Tu),
// as for one gas), and a lighter gas rises by bulk buoyancy, where the gas in the window above is
// denser than in the window below. Then *parcel is set to the density half-way between the two,
// which bounds the heavy parcel the cell is lifted through (gas_lift); else it is 0. `scales`:
// how many of the bulk window heights (4, 8, BULK_TALL rows) to try.
static uint32_t buoyancy_p(const World *w, int x, int y, ElementId self, ElementId up, int scales,
                           float *parcel) {
    *parcel = 0.0f;
    if (!elem_is_fluid(up)) {
        return 0;
    }
    float ts = elem_temp(self, w->temp[world_index(w, x, y)]);
    float tu = elem_temp(up, w->temp[world_index(w, x, y - 1)]);
    if (up != self && g_elem_behavior[up] == BEHAVIOR_GAS &&
        g_elem_behavior[self] == BEHAVIOR_GAS) {
        uint32_t p = 0;
        if (ts > tu + CONVECTION_MIN_K) {
            p = (uint32_t)((float)K_BUOY_16 * (ts - tu) / (ts + tu));
        }
        if (g_buoy_p[up][self] > 0 && gas_is_bubble(w, x, y, self)) {
            // A bubble: lifted to the top of the heavy gas around it.
            *parcel = 0.5f * (g_gas_buoy_density[self] + g_gas_buoy_density[up]);
            return PR_MAX(p, (uint32_t)g_buoy_p[up][self]);
        }
        if (scales > 0 && g_buoy_p[up][self] > 0) {
            // The parcel: the gas in the window above (7 wide). It sinks where it is denser than
            // the gas in the window below (a heavy layer on a light one turns over), or than the
            // gas in the lighter of the 7-wide windows on either side (a heavy blob or column in
            // lighter gas sinks). Windows are 4, 8 and 16 rows tall (above and below, each side
            // stopping at a cell of the column that is not gas): a sharp edge shows in the small
            // ones, a smeared one only in the tall ones, which have less noise.
            static const int k_h[3] = {4, 8, BULK_TALL};
            BulkSum sa = {0}, sb = {0}, sl = {0}, sr = {0};
            int h = 0;
            bool open_a = true, open_b = true;
            for (int k = 0; k < scales; k++) {
                for (; h < k_h[k]; h++) {
                    open_a = open_a && g_elem_behavior[world_elem(w, x, y - 1 - h)] == BEHAVIOR_GAS;
                    open_b = open_b && g_elem_behavior[world_elem(w, x, y + h)] == BEHAVIOR_GAS;
                    if (open_a) {
                        int ry = y - 1 - h;
                        bulk_row(w, x - BULK_HALF_W, x + BULK_HALF_W, ry, &sa);
                        bulk_row(w, x - 3 * BULK_HALF_W - 1, x - BULK_HALF_W - 1, ry, &sl);
                        bulk_row(w, x + BULK_HALF_W + 1, x + 3 * BULK_HALF_W + 1, ry, &sr);
                    }
                    if (open_b) {
                        bulk_row(w, x - BULK_HALF_W, x + BULK_HALF_W, y + h, &sb);
                    }
                }
                const BulkSum *side =
                    sr.n == 0 || (sl.n > 0 && bulk_mean(&sl) < bulk_mean(&sr)) ? &sl : &sr;
                float contrast = bulk_excess(&sa, &sb, BULK_Z);
                float light = bulk_mean(&sb);
                float cs = bulk_excess(&sa, side, BULK_Z * SIDE_NOISE);
                if (cs > contrast) {
                    contrast = cs;
                    light = bulk_mean(side);
                }
                if (contrast > 0.0f) {
                    float q = PR_MIN(1.0f, BULK_K * BULK_EVERY * contrast);
                    uint32_t pb = (uint32_t)((float)K_BUOY_16 * q);
                    if (pb > p) {
                        p = pb;
                        *parcel = 0.5f * (bulk_mean(&sa) + light);
                    }
                    break;
                }
                if (!open_a && !open_b) {
                    break;
                }
            }
        }
        return p;
    }
    if (fabsf(ts - tu) < CONVECTION_MIN_K) {
        // Same temperature: it cancels out of the density ratio; use the precomputed table.
        return up == self ? 0 : g_buoy_p[up][self];
    }
    float ds = elem_fluid_density(self, ts), du = elem_fluid_density(up, tu);
    if (du <= ds) {
        return 0;
    }
    return (uint32_t)((float)K_BUOY_16 * (du - ds) / (du + ds));
}

// Bulk buoyancy moves the heavy parcel above (x, y) down one cell as a whole: the light cell at
// (x, y) is lifted to the top of the parcel (up the column while the cells are denser gases, or
// the 3 cells of the row have a mean density of at least `parcel`; at most PARCEL_MAX) and the
// column of the parcel shifts down by one. So a blob of
// heavy gas sinks intact rather than crumbling one cell at a time, and a heavy layer on top of a
// light one turns over (the light gas lifted into it mixes in).
static void gas_lift(TickCtx *c, int x, int y, float parcel) {
    World *w = c->w;
    ElementId self = world_elem(w, x, y);
    int top = y - 1; // the cell above is in the parcel
    while (y - top < PARCEL_MAX && top > 0) {
        int ny = top - 1;
        ElementId e = world_elem(w, x, ny);
        if (g_elem_behavior[e] != BEHAVIOR_GAS ||
            (g_buoy_p[e][self] == 0 && bulk_row_mean(w, x - 1, x + 1, ny) < parcel)) {
            break;
        }
        top = ny;
    }
    for (int ty = y; ty > top; ty--) {
        swap_cells(c, x, ty, x, ty - 1);
    }
    gas_wake_bulk(c, x, top, x, y);
}

// Bulk buoyancy sideways (a gravity current): a denser gas next to the lighter gas at (x, y),
// which rests on something, trades places with it where the gas in the SPREAD_W x SPREAD_H cells
// from the heavy cell on (at and above this row) is significantly denser than on this side. So a
// heavy pile flattens out, and heavy gas pools in pits. (Bigger windows and a stricter test than
// for rising: this move does not lower the gas, so nothing would settle a mixture it acted on.)
// Returns the probability (x/65536) for the side s (-1 or +1); 0 if it does not apply.
static uint32_t gas_spread_p(const World *w, int x, int y, int s, ElementId self) {
    ElementId n = world_elem(w, x + s, y);
    if (n == self || g_elem_behavior[n] != BEHAVIOR_GAS || g_buoy_p[n][self] == 0) {
        return 0;
    }
    // The light cell rests on something (not void nor a gas lighter than the heavy one): the
    // heavy gas spreads along a floor, or over more heavy gas; a falling blob does not shed its
    // sides (there is light gas under the light gas beside it).
    ElementId under = world_elem(w, x, y + 1);
    if (under == ELEM_VOID || (g_elem_behavior[under] == BEHAVIOR_GAS && g_buoy_p[n][under] > 0)) {
        return 0;
    }
    int xh0 = s > 0 ? x + 1 : x - SPREAD_W, xl0 = s > 0 ? x - SPREAD_W + 1 : x;
    BulkSum bh = bulk_window(w, xh0, xh0 + SPREAD_W - 1, y - SPREAD_H + 1, y);
    BulkSum bl = bulk_window(w, xl0, xl0 + SPREAD_W - 1, y - SPREAD_H + 1, y);
    float contrast = bulk_excess(&bh, &bl, SPREAD_Z);
    return (uint32_t)((float)K_BUOY_16 * PR_MIN(1.0f, SPREAD_EVERY * contrast));
}

// Might bulk buoyancy or spreading move the gas `self` at (x, y)? Cheap: a denser gas is above it,
// or beside it.
static bool gas_bulk_gate(const World *w, int x, int y, ElementId self, ElementId up) {
    if (g_elem_behavior[up] == BEHAVIOR_GAS && g_buoy_p[up][self] > 0) {
        return true;
    }
    for (int s = -1; s <= 1; s += 2) {
        ElementId n = world_elem(w, x + s, y);
        if (g_elem_behavior[n] == BEHAVIOR_GAS && g_buoy_p[n][self] > 0) {
            return true;
        }
    }
    return false;
}

// ---- Gases (M4): mass and pressure. docs/SIMULATION.md -> Gases ----

// Farthest cell of the same gas that a gas cell equalises its pressure with in one tick. Pressure
// spreads by diffusion; long jumps make it spread over a room in seconds instead of minutes.
#define GAS_REACH 16
// ... but only with a cell within this temperature difference (K); otherwise its neighbour.
#define GAS_JUMP_MAX_DT 2.0f
// Neighbouring gas cells whose pressures differ by less than this fraction count as equal: no
// transfer, and the cells may sleep.
#define GAS_EQ_TOL 1e-3f
// A gas pushes a neighbouring gas cell out of the way only if its pressure is more than this many
// times higher. Below 2 the push would leave the two cells it shares at a lower pressure than the
// gas it pushed (now squeezed into its neighbour), and the next tick would push back.
#define GAS_PUSH_RATIO 2.1f
// Below that, a gas more than this fraction above the pressure of a different gas next to it takes
// that cell over, spreading the change over both gases' regions (world_gas_region): its gas is
// squeezed into the cells of its kind around, and the cell is refilled from the cells of the
// higher-pressure gas around. Total pressure thus evens out across different gases.
#define GAS_BALANCE_TOL 0.01f
// A cell stays awake for that only above this ratio, so small regions where the move would
// overshoot (and is refused) do not stay awake for ever.
#define GAS_BALANCE_WAKE 1.03f
// Regions for it are smaller than for liquid (it runs in the parallel pass, often in mixed gas),
// and it is tried with this probability (x/65536) per tick when it applies.
#define GAS_BALANCE_RADIUS 8
#define GAS_BALANCE_CELLS  64
#define GAS_BALANCE_P16    4096u

static const int8_t k_dir4[4][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};

// Proportional to pressure (P = pv * R / V): moles times temperature.
static inline float gas_pv(const World *w, size_t i, ElementId e) {
    return w->mass[i] * elem_temp(e, w->temp[i]) * g_gas_inv_molar[e];
}

static inline bool gas_differs(float a, float b) {
    return fabsf(a - b) > GAS_EQ_TOL * PR_MAX(a, b);
}

// Adds mass dm of the same gas, at stored temperature ts, to cell r. Same gas, same heat
// capacity: the stored temperatures (enthalpies) mix by mass, conserving energy.
static inline void gas_add(World *w, size_t r, float dm, float ts) {
    float mr = w->mass[r];
    w->temp[r] = (mr * w->temp[r] + dm * ts) / (mr + dm);
    w->mass[r] = mr + dm;
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

// Puts a new gas cell like `like`, holding m kg at stored temperature ts, at (x, y). It counts as
// updated this tick.
static inline void gas_spawn(TickCtx *c, int x, int y, Cell like, float m, float ts) {
    World *w = c->w;
    PR_ASSERT(in_reach(c, x, y));
    size_t i = world_index(w, x, y);
    like.stamp = c->stamp;
    like.shade = (uint8_t)rng_next(&c->rng);
    like.aux = 0;
    w->cells[i] = like;
    w->mass[i] = m;
    w->temp[i] = ts;
    ctx_arrive(c, x, y, like, ts);
}

static inline void gas_clear(World *w, size_t i) {
    w->cells[i].element = ELEM_VOID;
    w->cells[i].aux = 0;
    w->mass[i] = 0.0f;
    w->temp[i] = 0.0f;
}

// Diffusion (docs/SIMULATION.md -> Gas mixtures). The gas `a` at (x, y) trades places with the
// different gas `b` next to it, in direction k, when there is clearly more of `a` on its own side
// than on b's side: concentration flows down its gradient, through a clump's inside too, until the
// gases are evenly mixed, and then nothing moves (the region sleeps). Each side is a window
// across the direction (2 * k_diffuse_half_w[s] + 1 wide) and k_diffuse_d[s] cells along it,
// sampled every k_diffuse_stride[s] cells: the short windows see a sharp edge, the long, wide ones
// (27 to 240 samples, so less noise) a weak or smeared difference (up to 32 cells, the reach).
// `a` and `b` themselves are not counted. "Clearly": the fractions of `a` differ by DIFFUSE_Z
// standard errors of a common mixture (pooled), and `b` is not less on its own side.
#define DIFFUSE_SCALES 4
#define DIFFUSE_Z      3.0f
static const int8_t k_diffuse_d[DIFFUSE_SCALES] = {4, 8, 16, 32};
static const int8_t k_diffuse_stride[DIFFUSE_SCALES] = {1, 1, 2, 2};
static const int8_t k_diffuse_half_w[DIFFUSE_SCALES] = {3, 3, 5, 7};

// The swap probability factor (0..1) for `a` at (x, y) and its neighbour in direction k: the
// gradient of the fraction of `a` (among gas cells) between the two sides, at the shortest scale
// where it is clear (0 if none). The swap probability scales with it (Fick).
// Cheap gate for gas_gradient: does the fraction of `a` in the 8 x 8 block of (x, y) differ from
// that of a block 1 to 4 blocks away (left, right, up or down) by GATE_Z standard errors? If not,
// there is no gradient worth the precise test (evenly mixed gas, like still air, costs almost
// nothing). Block counts are a tick old at most (gas_blocks_update).
#define GATE_Z 2.0f

static bool gas_blocks_differ(const World *w, int x, int y, ElementId a) {
    int bw = w->width >> GAS_BLOCK_SHIFT, bh = w->height >> GAS_BLOCK_SHIFT;
    int bx = x >> GAS_BLOCK_SHIFT, by = y >> GAS_BLOCK_SHIFT, slot = g_gas_slot[a];
    const uint8_t *b0 = &w->gas_blocks[(size_t)(by * bw + bx) * GAS_SLOTS];
    if (b0[0] < 8) {
        return true; // mostly not gas here: leave it to the precise test
    }
    for (int d = 1; d <= 4; d++) {
        for (int k = 0; k < 4; k++) {
            int nx = bx + k_dir4[k][0] * d, ny = by + k_dir4[k][1] * d;
            if (nx < 0 || ny < 0 || nx >= bw || ny >= bh) {
                continue;
            }
            const uint8_t *b1 = &w->gas_blocks[(size_t)(ny * bw + nx) * GAS_SLOTS];
            if (b1[0] < 8) {
                continue;
            }
            float n0 = (float)b0[0], n1 = (float)b1[0];
            float diff = (float)b0[slot] / n0 - (float)b1[slot] / n1;
            float p = (float)(b0[slot] + b1[slot]) / (n0 + n1);
            if (diff * diff > GATE_Z * GATE_Z * p * (1.0f - p) * (1.0f / n0 + 1.0f / n1)) {
                return true;
            }
        }
    }
    return false;
}

static float gas_gradient(const World *w, int x, int y, int k, ElementId a, ElementId b) {
    if (!gas_blocks_differ(w, x, y, a)) {
        return 0.0f;
    }
    int dx = k_dir4[k][0], dy = k_dir4[k][1];
    int ax = -dy, ay = dx; // across
    for (int s = 0; s < DIFFUSE_SCALES; s++) {
        int d = k_diffuse_d[s], st = k_diffuse_stride[s], hw = k_diffuse_half_w[s];
        int na[2] = {0, 0}, nb[2] = {0, 0}, n[2] = {0, 0};
        for (int t = 0; t < d; t += st) {
            for (int q = -hw; q <= hw; q++) {
                if (t == 0 && q == 0) {
                    continue; // a and b themselves
                }
                for (int side = 0; side < 2; side++) {
                    int along = side == 0 ? -t : 1 + t;
                    ElementId e = world_elem(w, x + dx * along + ax * q, y + dy * along + ay * q);
                    if (g_elem_behavior[e] == BEHAVIOR_GAS) {
                        n[side]++;
                        na[side] += e == a;
                        nb[side] += e == b;
                    }
                }
            }
        }
        if (n[0] < 8 || n[1] < 8) {
            continue; // mostly walls
        }
        float fa0 = (float)na[0] / (float)n[0], fa1 = (float)na[1] / (float)n[1];
        float diff = fa0 - fa1;
        if (diff <= 0.0f || (float)nb[1] * (float)n[0] < (float)nb[0] * (float)n[1]) {
            continue;
        }
        float p = (float)(na[0] + na[1]) / (float)(n[0] + n[1]);
        float var = p * (1.0f - p) * (1.0f / (float)n[0] + 1.0f / (float)n[1]);
        if (diff * diff > DIFFUSE_Z * DIFFUSE_Z * var) {
            // Fick: the swaps that can happen here (a on this side, b on that) are a fraction
            // p (1 - p) of the pairs, so dividing by it makes the flow proportional to the
            // gradient alone, whatever the concentration (dilute haze spreads as fast as a
            // strong mix).
            return PR_MIN(1.0f, diff / (float)d / PR_MAX(4.0f * p * (1.0f - p), 0.05f));
        }
    }
    return 0.0f;
}

// The cells within 2 steps (Manhattan) of (x, y): nb[2 + dy][2 + dx]; corners unused.
typedef struct GasNeighbourhood {
    ElementId e[5][5];
} GasNeighbourhood;

static inline void gas_load_neighbourhood(const World *w, int x, int y, GasNeighbourhood *nb) {
    if (PR_LIKELY(x >= 2 && y >= 2 && x + 2 < w->width && y + 2 < w->height)) {
        for (int dy = -2; dy <= 2; dy++) {
            int span = 2 - (dy < 0 ? -dy : dy);
            const Cell *row = &w->cells[world_index(w, x, y + dy)];
            for (int dx = -span; dx <= span; dx++) {
                nb->e[2 + dy][2 + dx] = row[dx].element;
            }
        }
        return;
    }
    for (int dy = -2; dy <= 2; dy++) {
        int span = 2 - (dy < 0 ? -dy : dy);
        for (int dx = -span; dx <= span; dx++) {
            nb->e[2 + dy][2 + dx] = world_elem(w, x + dx, y + dy);
        }
    }
}

// Evening out at the smallest scale: does trading places with the different gas in direction k
// lower the number of same-gas contacts around the two cells? (The cell itself is nb->e[2][2].)
// It breaks up small clusters (a few cells of one gas side by side) that are too small to show as
// a gradient. Every such swap lowers that number, so it comes to rest.
static inline bool gas_mix_gain(const GasNeighbourhood *nb, int k) {
    int dx = k_dir4[k][0], dy = k_dir4[k][1];
    ElementId a = nb->e[2][2], b = nb->e[2 + dy][2 + dx];
    // Same-gas contacts around each of the two places, as a share of the gas cells there (walls
    // count for neither: a cell against a wall is no worse placed than one in the open).
    int like_a = 0, other_a = 0, gas_a = 0; // around the cell, except the neighbour
    for (int j = 0; j < 4; j++) {
        if (j != k) {
            ElementId r = nb->e[2 + k_dir4[j][1]][2 + k_dir4[j][0]];
            like_a += r == a;
            other_a += r == b;
            gas_a += g_elem_behavior[r] == BEHAVIOR_GAS;
        }
    }
    // Around the neighbour, except the cell: straight on, and both sides.
    int like_b = 0, other_b = 0, gas_b = 0;
    const int q[3][2] = {{2 * dx, 2 * dy}, {dx - dy, dy + dx}, {dx + dy, dy - dx}};
    for (int j = 0; j < 3; j++) {
        ElementId r = nb->e[2 + q[j][1]][2 + q[j][0]];
        like_b += r == b;
        other_b += r == a;
        gas_b += g_elem_behavior[r] == BEHAVIOR_GAS;
    }
    if (gas_a == 0 || gas_b == 0) {
        return false;
    }
    // before: like_a / gas_a + like_b / gas_b; after: other_a / gas_a + other_b / gas_b.
    return (other_a - like_a) * gas_b + (other_b - like_b) * gas_a < 0;
}

// May the gases at (x, y) and its neighbour in direction k, of different kinds, trade places to
// mix (gas_mix_gain)?
static bool gas_mix_ok(const World *w, int x, int y, int k) {
    GasNeighbourhood nb;
    gas_load_neighbourhood(w, x, y, &nb);
    return gas_mix_gain(&nb, k);
}

// Where can the gas at (nx, ny), pushed from the direction (dx, dy), go? Straight on, then to
// either side: a cell of the same gas (it merges into it) or void (it moves there). Returns false
// if neither.
static bool gas_push_target(const World *w, int nx, int ny, int dx, int dy, int side, int *kx,
                            int *ky) {
    ElementId other = world_elem(w, nx, ny);
    const int d[3][2] = {{dx, dy}, {-dy * side, dx * side}, {dy * side, -dx * side}};
    for (int j = 0; j < 3; j++) {
        int qx = nx + d[j][0], qy = ny + d[j][1];
        ElementId q = world_elem(w, qx, qy);
        if (q == other || q == ELEM_VOID) {
            *kx = qx;
            *ky = qy;
            return true;
        }
    }
    return false;
}

// The gas at (x, y), at more than GAS_PUSH_RATIO times the pressure of the gas next to it (of any
// kind), pushes that gas into its own neighbour and expands into the freed cell (half its mass
// each).
static bool gas_push(TickCtx *c, int x, int y, int dx, int dy) {
    World *w = c->w;
    int nx = x + dx, ny = y + dy, kx, ky;
    if (!gas_push_target(w, nx, ny, dx, dy, rng_bit(&c->rng) ? 1 : -1, &kx, &ky)) {
        return false;
    }
    PR_ASSERT(in_reach(c, kx, ky));
    size_t i = world_index(w, x, y), ni = world_index(w, nx, ny), ki = world_index(w, kx, ky);
    Cell other = w->cells[ni];
    if (w->cells[ki].element == ELEM_VOID) {
        other.stamp = c->stamp;
        w->cells[ki] = other;
        w->mass[ki] = w->mass[ni];
        w->temp[ki] = w->temp[ni];
    } else {
        gas_add(w, ki, w->mass[ni], w->temp[ni]);
    }
    ctx_arrive(c, kx, ky, other, w->temp[ki]);
    float half = w->mass[i] * 0.5f;
    w->mass[i] = half;
    gas_spawn(c, nx, ny, w->cells[i], half, w->temp[i]);
    gas_wake(c, x, y, kx, ky);
    c->stats.cells_moved++;
    return true;
}

// The gas at (x, y) is above the pressure of the different gas at (nx, ny) by more than
// GAS_BALANCE_TOL: that cell changes over to this gas, the change spread over both regions. Only
// if the regions' mean pressures agree, and if afterwards this gas is still at least the other's
// pressure, so it never flips back. Returns true if done.
static bool gas_balance(TickCtx *c, int x, int y, int nx, int ny) {
    World *w = c->w;
    size_t i = world_index(w, x, y), ni = world_index(w, nx, ny);
    ElementId a = w->cells[i].element, b = w->cells[ni].element;
    int32_t fb[GAS_BALANCE_CELLS], fa[GAS_BALANCE_CELLS];
    int nb = world_gas_region(w, nx, ny, b, nx, ny, GAS_BALANCE_RADIUS, GAS_BALANCE_CELLS, fb);
    if (nb == 0) {
        return false;
    }
    int na = world_gas_region(w, nx, ny, a, nx, ny, GAS_BALANCE_RADIUS, GAS_BALANCE_CELLS, fa);
    if (na == 0) {
        return false;
    }
    // Judge by the regions' mean pressures: a single cell out of line (just squeezed or thinned)
    // must not shift whole regions.
    float pa = 0.0f, pb = 0.0f;
    for (int k = 0; k < na; k++) {
        pa += gas_pv(w, (size_t)fa[k], a);
    }
    for (int k = 0; k < nb; k++) {
        pb += gas_pv(w, (size_t)fb[k], b);
    }
    pa /= (float)na;
    pb /= (float)nb;
    if (pa <= (1.0f + GAS_BALANCE_TOL) * pb ||
        pa * (float)na / (float)(na + 1) < pb * (float)(nb + 1) / (float)nb) {
        return false;
    }
    PR_ASSERT(in_reach(c, nx - GAS_BALANCE_RADIUS, ny - GAS_BALANCE_RADIUS) &&
              in_reach(c, nx + GAS_BALANCE_RADIUS, ny + GAS_BALANCE_RADIUS));
    world_gas_spread_add(w, fb, nb, w->mass[ni], w->temp[ni]);
    for (int k = 0; k < nb; k++) {
        ctx_arrive(c, fb[k] % w->width, fb[k] / w->width, w->cells[fb[k]], w->temp[fb[k]]);
    }
    float t;
    float m = world_gas_spread_take(w, fa, na, &t);
    Cell cell = w->cells[i];
    cell.stamp = c->stamp;
    cell.aux = 0;
    w->cells[ni] = cell;
    w->mass[ni] = m;
    w->temp[ni] = t;
    ctx_arrive(c, nx, ny, cell, t);
    ctx_wake(c, nx - GAS_BALANCE_RADIUS, ny - GAS_BALANCE_RADIUS, nx + GAS_BALANCE_RADIUS,
             ny + GAS_BALANCE_RADIUS, true, true);
    c->stats.cells_moved++;
    return true;
}

// Pressure equalisation: the cell trades mass with a cell of the same gas so that both end at the
// same pressure. The partner is the last cell of the same gas met walking a random 1..GAS_REACH
// cells from (x, y) in direction (dx, dy) through gas of any kind, if it has nearly the same
// temperature; otherwise the adjacent cell. The moved gas carries its heat, which mixes into the
// receiver: long jumps between cells at different temperatures would smear heat across the room
// and smother convection. Returns true if mass moved.
static bool gas_equalize(TickCtx *c, int x, int y, int dx, int dy) {
    World *w = c->w;
    size_t i = world_index(w, x, y);
    ElementId e = w->cells[i].element;
    float t = elem_temp(e, w->temp[i]);
    int reach = 1 + (int)rng_range(&c->rng, GAS_REACH);
    int px = x, py = y;
    for (int s = 1, qx = x, qy = y; s <= reach; s++) {
        qx += dx;
        qy += dy;
        ElementId q = world_elem(w, qx, qy);
        if (g_elem_behavior[q] != BEHAVIOR_GAS) {
            break;
        }
        if (q == e) {
            px = qx;
            py = qy;
        }
    }
    if (px == x && py == y) {
        return false;
    }
    if (fabsf(elem_temp(e, w->temp[world_index(w, px, py)]) - t) > GAS_JUMP_MAX_DT) {
        px = x + dx;
        py = y + dy;
        if (world_elem(w, px, py) != e) {
            return false;
        }
    }
    PR_ASSERT(in_reach(c, px, py));
    size_t pi = world_index(w, px, py);
    float m = w->mass[i], mp = w->mass[pi];
    float tp = elem_temp(e, w->temp[pi]);
    float a = m * t, b = mp * tp;
    if (!gas_differs(a, b)) {
        return false;
    }
    // Beyond the push ratio the denser side pushes instead (2b), unless it is thin gas (in
    // vacuum-like conditions any transfer is fine).
    if ((a > GAS_PUSH_RATIO * b && m >= g_gas_split_mass[e]) ||
        (b > GAS_PUSH_RATIO * a && mp >= g_gas_split_mass[e])) {
        return false;
    }
    // Equal pressure afterwards (same gas). The moved gas keeps the sender's temperature, so the
    // receiver's m * T grows by dm * t_sender: (m - dm) * t == mp * tp + dm * t when i sends.
    float dm = a > b ? (a - b) / (2.0f * t) : (a - b) / (2.0f * tp);
    if (dm > 0.0f) {
        gas_add(w, pi, dm, w->temp[i]);
        w->mass[i] = m - dm;
        ctx_arrive(c, px, py, w->cells[pi], w->temp[pi]);
    } else {
        gas_add(w, i, -dm, w->temp[pi]);
        w->mass[pi] = mp + dm;
        ctx_arrive(c, x, y, w->cells[i], w->temp[i]);
    }
    gas_wake(c, x, y, px, py);
    c->stats.gas_transfers++;
    return true;
}

// Could the gas at (x, y) still do something? Stay awake if so. Cheap checks first. Next to a
// different gas, diffusion is checked only on a tick that tries everything (`full`, every
// BULK_FULL_EVERY ticks); until then the cell stays awake.
static bool gas_is_mobile(const World *w, int x, int y, ElementId e, uint32_t p_up, bool full) {
    size_t i = world_index(w, x, y);
    float m = w->mass[i];
    float pv = gas_pv(w, i, e);
    bool dense = m >= g_gas_split_mass[e];
    uint8_t other = 0; // directions with a different gas
    for (int k = 0; k < 4; k++) {
        int dx = k_dir4[k][0], dy = k_dir4[k][1];
        int nx = x + dx, ny = y + dy, kx, ky;
        ElementId n = world_elem(w, nx, ny);
        if (n == ELEM_VOID) {
            return true; // expand into it (split, or move as a particle)
        }
        if (g_elem_behavior[n] != BEHAVIOR_GAS) {
            continue;
        }
        size_t ni = world_index(w, nx, ny);
        float pvn = gas_pv(w, ni, n);
        if (dense && pv > GAS_PUSH_RATIO * pvn && gas_push_target(w, nx, ny, dx, dy, 1, &kx, &ky)) {
            return true;
        }
        if (n == e) {
            if (gas_differs(pv, pvn) || m + w->mass[ni] < g_gas_split_mass[e]) {
                return true;
            }
        } else {
            if (dense && pv > GAS_BALANCE_WAKE * pvn) {
                return true;
            }
            other |= (uint8_t)(1u << k);
        }
    }
    if (p_up > 0) {
        return true;
    }
    if (other && !full) {
        return true;
    }
    if (other) {
        GasNeighbourhood nb;
        gas_load_neighbourhood(w, x, y, &nb);
        for (int k = 0; k < 4; k++) {
            if (((other >> k) & 1u) && gas_mix_gain(&nb, k)) {
                return true;
            }
        }
    }
    for (int k = 0; k < 4; k++) {
        if (((other >> k) & 1u) &&
            gas_gradient(w, x, y, k, e, world_elem(w, x + k_dir4[k][0], y + k_dir4[k][1])) > 0.0f) {
            return true;
        }
    }
    return false;
}

// ---- Reactions (M5). docs/SIMULATION.md -> Reactions ----

#define REACT_REACH 3 // farthest partner cell (square rings)

// The gas at (x, y), if it is the fuel of a reaction, reacts with a nearby cell of the right kind
// when hot enough (itself or any of its neighbours). Mass is conserved exactly:
// with enough of the other, this cell becomes the product, holding both masses used; with too
// little, the other cell becomes the product and this keeps what is left. The heat released (and
// the reactants' own) heats the product, up to FLAME_MAX_K. Returns true if it reacted.
static bool react(TickCtx *c, int x, int y) {
    World *w = c->w;
    size_t i = world_index(w, x, y);
    ElementId a = w->cells[i].element;
    const Reaction *rx = &g_reactions[g_reaction_of[a] - 1];
    bool hot = elem_temp(a, w->temp[i]) >= rx->ignite_k;
    for (int k = 0; k < 4 && !hot; k++) {
        int qx = x + k_dir4[k][0], qy = y + k_dir4[k][1];
        ElementId n = world_elem(w, qx, qy);
        hot = n != ELEM_OUT && g_elem_cap[n] > 0.0f &&
              elem_temp(n, w->temp[world_index(w, qx, qy)]) >= rx->ignite_k;
    }
    if (!hot) {
        return false; // cold: nothing to do (cheap for the common case)
    }
    // The partner: the nearest cell of `b` within REACT_REACH (square rings, from a random
    // corner). Burning gases mix at the molecular level; with one gas per cell, the push of the
    // hot products would otherwise leave fuel and oxidiser in pockets that never touch.
    int nx = 0, ny = 0;
    bool found = false;
    int rot = (int)rng_range(&c->rng, 8);
    for (int r = 1; r <= REACT_REACH && !found; r++) {
        int side = 2 * r, ring = 4 * side;
        for (int k = 0; k < ring && !found; k++) {
            int j = (k + rot * r) % ring, s4 = j / side, o = j % side;
            int qx = s4 == 0 ? x - r + o : s4 == 1 ? x + r : s4 == 2 ? x + r - o : x - r;
            int qy = s4 == 0 ? y - r : s4 == 1 ? y - r + o : s4 == 2 ? y + r : y + r - o;
            if (world_elem(w, qx, qy) == rx->b) {
                nx = qx;
                ny = qy;
                found = true;
            }
        }
    }
    if (!found) {
        return false;
    }
    size_t ni = world_index(w, nx, ny);
    float ma = w->mass[i], mb = w->mass[ni];
    float burn_a = PR_MIN(ma, mb / rx->b_per_a); // kg of a that react
    float use_b = burn_a * rx->b_per_a;
    float cpa = g_elem_cp[a], cpb = g_elem_cp[rx->b], cpp = g_elem_cp[rx->product];
    float energy = burn_a * cpa * w->temp[i] + use_b * cpb * w->temp[ni] + burn_a * rx->heat_per_kg;
    float mp = burn_a + use_b;
    float tp = PR_MIN(energy / (mp * cpp), FLAME_MAX_K);
    Cell product = w->cells[i];
    product.element = rx->product;
    product.stamp = c->stamp;
    product.aux = 0;
    // The product takes the cell that is used up: this one if all of it reacted, else the other.
    size_t pi = burn_a >= ma ? i : ni, rest = pi == i ? ni : i;
    float rest_mass = pi == i ? mb - use_b : ma - burn_a;
    product.shade = w->cells[pi].shade;
    w->cells[pi] = product;
    w->mass[pi] = mp;
    w->temp[pi] = tp;
    if (rest_mass <= 0.0f) { // both used up exactly: the other cell empties
        w->cells[rest].element = ELEM_VOID;
        w->temp[rest] = 0.0f;
        w->mass[rest] = 0.0f;
    } else {
        w->mass[rest] = rest_mass;
    }
    int px = pi == i ? x : nx, py = pi == i ? y : ny;
    ctx_arrive(c, px, py, product, tp);
    ctx_wake(c, PR_MIN(x, nx) - 1, PR_MIN(y, ny) - 1, PR_MAX(x, nx) + 1, PR_MAX(y, ny) + 1, true,
             false);
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

// Falling matter (liquid or powder) kicks the gas it passes aside and drags the gas beside it
// down: the current's speed is FALL_WIND_BASE plus its fall speed (1/16 cells per tick) / 4.
#define FALL_WIND_BASE 6

// The gas cell at (x, y), now holding gas that a falling cell at fall speed `speed` displaced.
static void stir_by_fall(TickCtx *c, int x, int y, int speed) {
    World *w = c->w;
    int s = FALL_WIND_BASE + speed / 4;
    int side = rng_bit(&c->rng) ? 1 : -1;
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
static bool gas_follow_wind(TickCtx *c, int x, int y) {
    World *w = c->w;
    Cell *cell = cell_ptr(w, x, y);
    int s = gas_wind_speed(cell->aux);
    if (s == 0) {
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
            if (t != ELEM_VOID &&
                g_gas_buoy_density[t] != g_gas_buoy_density[cell_ptr(w, tx, ty)->element]) {
                gas_wake_bulk(c, PR_MIN(x, tx), PR_MIN(y, ty), PR_MAX(x, tx), PR_MAX(y, ty));
            }
            return true;
        }
        // Blocked: turn 45 degrees either way.
        cell->aux = gas_wind((d + (rng_bit(&c->rng) ? 1 : 7)) & 7, s2 - WIND_TURN_LOSS);
        return false;
    }
    cell->aux = gas_wind(d, s2);
    return false;
}

static void update_gas(TickCtx *c, int x, int y) {
    World *w = c->w;
    size_t i = world_index(w, x, y);
    Cell self = w->cells[i];
    ElementId e = self.element;

    // Currents first: moving air carries the gas along.
    if (self.aux != 0 && gas_follow_wind(c, x, y)) {
        return;
    }

    // 0. Reactions (burning), only where something is hot enough to ignite.
    if (c->may_react && g_reaction_of[e] && react(c, x, y)) {
        return;
    }

    // 1. Buoyancy: rise through a denser fluid above (including colder gas of the same kind:
    //    warm air rises). Densities at 1 atm: pressure is handled below.
    //    Bulk buoyancy (costly window sums): 4-row windows one tick in BULK_EVERY, all (up to 450
    //    reads) every BULK_FULL_EVERY ticks.
    float parcel;
    ElementId up = world_elem(w, x, y - 1);
    int scales = 0; // window heights tried: none, 4 rows, or all
    if ((w->tick + (uint64_t)(c->cx * 5 + c->cy * 3)) % BULK_FULL_EVERY == 0) {
        scales = 3;
    } else if (rng_range(&c->rng, BULK_EVERY) == 0) {
        scales = 1;
    }
    uint32_t p_up = buoyancy_p(w, x, y, e, up, scales, &parcel);
    if (p_up > 0 && rng_chance16(&c->rng, p_up)) {
        if (parcel > 0.0f) {
            gas_lift(c, x, y, parcel);
        } else {
            swap_cells(c, x, y, x, y - 1);
        }
        return;
    }
    // 1b. Heavy gas beside spreads under it (both sides, in random order): one tick in
    //     SPREAD_EVERY, and with the full bulk test.
    int s0 = rng_bit(&c->rng) ? 1 : -1;
    bool spread = scales == 3 || rng_range(&c->rng, SPREAD_EVERY) == 0;
    for (int j = 0; j < (spread ? 2 : 0); j++) {
        int s = j ? -s0 : s0;
        uint32_t ps = gas_spread_p(w, x, y, s, e);
        p_up |= ps;
        if (ps > 0 && rng_chance16(&c->rng, ps)) {
            swap_cells(c, x, y, x + s, y);
            gas_wake_bulk(c, PR_MIN(x, x + s), y, PR_MAX(x, x + s), y);
            return;
        }
    }

    // 2. One random direction per tick.
    float m = w->mass[i];
    bool dense = m >= g_gas_split_mass[e];
    int r = (int)rng_range(&c->rng, 4);
    int dx = k_dir4[r][0], dy = k_dir4[r][1];
    int nx = x + dx, ny = y + dy;
    ElementId n = world_elem(w, nx, ny);

    if (n == ELEM_VOID) {
        // 2a. Expansion into vacuum. Dense gas splits: half its mass goes 1..spread cells into
        //     the void, so the gas fills the space and thins out. Thin gas moves as a particle.
        int steps = dense ? 1 + (int)rng_range(&c->rng, g_elem_spread[e]) : g_elem_spread[e];
        int tx = x, ty = y;
        while (steps-- > 0 && world_elem(w, tx + dx, ty + dy) == ELEM_VOID) {
            tx += dx;
            ty += dy;
        }
        if (dense) {
            w->mass[i] = m * 0.5f;
            gas_spawn(c, tx, ty, self, m * 0.5f, w->temp[i]);
            gas_wake(c, x, y, tx, ty);
            c->stats.cells_moved++;
        } else {
            swap_cells(c, x, y, tx, ty);
        }
        return;
    }

    if (g_elem_behavior[n] == BEHAVIOR_GAS) {
        size_t ni = world_index(w, nx, ny);
        // 2b. Much higher pressure than the neighbour (of any gas): push it aside and expand
        //     into its cell. Hot gas expanding pushes the cold gas away rather than mixing its
        //     heat into it (which equalising alone would do).
        if (dense && gas_pv(w, i, e) > GAS_PUSH_RATIO * gas_pv(w, ni, n) &&
            gas_push(c, x, y, dx, dy)) {
            return;
        }
        if (n == e) {
            // 2c. Two thin cells of the same gas merge (thin gas stays a few particles rather
            //     than many ever-thinner ones).
            if (m + w->mass[ni] < g_gas_split_mass[e]) {
                gas_add(w, ni, m, w->temp[i]);
                ctx_arrive(c, nx, ny, w->cells[ni], w->temp[ni]);
                gas_clear(w, i);
                gas_wake(c, x, y, nx, ny);
                c->stats.cells_moved++;
                return;
            }
        } else {
            // 2d. A different gas at a lower pressure: take its cell over (spread out).
            if (dense && gas_pv(w, i, e) > (1.0f + GAS_BALANCE_TOL) * gas_pv(w, ni, n) &&
                rng_chance16(&c->rng, GAS_BALANCE_P16) && gas_balance(c, x, y, nx, ny)) {
                return;
            }
            // 2e. Else mix (trade places). Where the gas is moving (a current in either cell),
            //     turbulence: at random, with a probability up to P_GAS_MIX_STIRRED in proportion
            //     to the current's speed. In still gas, diffusion: down the concentration
            //     gradient, with a probability up to P_GAS_MIX in proportion to it
            //     (gas_gradient), or, with probability P_GAS_SPECKLE, where that breaks up a
            //     small cluster (gas_mix_gain).
            int wind = PR_MAX(gas_wind_speed(w->cells[i].aux), gas_wind_speed(w->cells[ni].aux));
            bool mix;
            if (wind > 0) {
                mix = rng_chance16(&c->rng, P_GAS_MIX_STIRRED_16 * (uint32_t)wind / GAS_WIND_MAX);
            } else {
                uint32_t roll = rng_next(&c->rng) >> 16;
                mix = (roll < P_GAS_SPECKLE_16 && gas_mix_ok(w, x, y, r)) ||
                      (roll < P_GAS_MIX_16 && rng_unit(&c->rng) < gas_gradient(w, x, y, r, e, n));
            }
            if (mix) {
                swap_cells(c, x, y, nx, ny);
                gas_wake_bulk(c, PR_MIN(x, nx), PR_MIN(y, ny), PR_MAX(x, nx), PR_MAX(y, ny));
                return;
            }
        }
        // 2f. Equalise pressure with the same gas in that direction (through other gases).
        if (gas_equalize(c, x, y, dx, dy)) {
            return;
        }
    }

    // 3. Did not move. Stay awake only if something is still possible.
    //    Bulk buoyancy was not fully tried this tick: stay awake while a lighter gas is under or
    //    beside a denser one, until a tick that tries it all says no.
    if (gas_is_mobile(w, x, y, e, p_up, scales == 3) ||
        (scales < 3 && gas_bulk_gate(w, x, y, e, up))) {
        keep_alive(c, x, y);
    }
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
            stir_by_fall(c, x, y, speed);
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

    // Bottom to top; horizontal direction alternates every tick (no left/right bias).
    bool left_to_right = (w->tick & 1) == 0;
    for (int ly = r.max_y; ly >= r.min_y; ly--) {
        int y = c->base_y + ly;
        Cell *row = &w->cells[world_index(w, 0, y)];
        for (int i = 0; i <= r.max_x - r.min_x; i++) {
            int x = c->base_x + (left_to_right ? r.min_x + i : r.max_x - i);
            Cell *cell = &row[x];
            uint8_t b = g_elem_behavior[cell->element];
            if (b <= BEHAVIOR_STATIC || cell->stamp == c->stamp) {
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

// Scratch memory for one phase: the chunks to update and one TickCtx per chunk.
typedef struct PhaseScratch {
    int *chunk_list; // every chunk (gas_blocks_update)
    TickCtx *ctx;
    int capacity;
} PhaseScratch;

static void phase_job(void *user, int index) {
    // Work on a copy on this thread's stack: jobs' contexts sit side by side in one array, and
    // updating them in place (every move touches the stats and wakes) made threads fight over
    // shared cache lines.
    TickCtx *slot = &((TickCtx *)user)[index];
    TickCtx c = *slot;
    update_chunk(&c);
    *slot = c;
}

static PhaseScratch *get_scratch(World *w) {
    int capacity = ((w->chunks_x + 1) / 2) * ((w->chunks_y + 1) / 2);
    if (!w->sim_scratch) {
        size_t bytes = sizeof(PhaseScratch) + (size_t)capacity * sizeof(TickCtx) +
                       (size_t)w->chunks_x * (size_t)w->chunks_y * sizeof(int);
        PhaseScratch *s = pr_aligned_alloc_zero(64, bytes + 64);
        PR_ASSERT_ALWAYS(s != NULL);
        s->capacity = capacity;
        s->ctx = (TickCtx *)(((uintptr_t)(s + 1) + 63) & ~(uintptr_t)63);
        s->chunk_list = (int *)(s->ctx + capacity);
        w->sim_scratch = s;
    }
    return w->sim_scratch;
}

// Gas block counts (World.gas_blocks) of the chunks awake this tick, recounted in parallel before
// the movement pass (each job writes only its own chunk's blocks). A chunk whose cells change is
// awake the next tick, so the counts are at most a tick old.
typedef struct GasBlockJob {
    World *w;
    const int *chunks;
} GasBlockJob;

static void gas_blocks_job(void *user, int index) {
    const GasBlockJob *job = user;
    World *w = job->w;
    int ci = job->chunks[index];
    int cx = ci % w->chunks_x, cy = ci / w->chunks_x;
    int bw = w->width >> GAS_BLOCK_SHIFT;
    for (int by = 0; by < CHUNK_SIZE / GAS_BLOCK; by++) {
        for (int bx = 0; bx < CHUNK_SIZE / GAS_BLOCK; bx++) {
            uint8_t counts[GAS_SLOTS] = {0};
            int x0 = (cx << CHUNK_SHIFT) + bx * GAS_BLOCK,
                y0 = (cy << CHUNK_SHIFT) + by * GAS_BLOCK;
            for (int y = y0; y < y0 + GAS_BLOCK; y++) {
                const Cell *row = &w->cells[world_index(w, x0, y)];
                for (int x = 0; x < GAS_BLOCK; x++) {
                    uint8_t slot = g_gas_slot[row[x].element];
                    counts[0] += slot != 0;
                    counts[slot] += slot != 0;
                }
            }
            size_t b = (size_t)((y0 >> GAS_BLOCK_SHIFT) * bw + (x0 >> GAS_BLOCK_SHIFT));
            memcpy(&w->gas_blocks[b * GAS_SLOTS], counts, GAS_SLOTS);
        }
    }
}

static void gas_blocks_update(World *w, int *list) {
    int count = 0;
    for (int i = 0; i < w->chunks_x * w->chunks_y; i++) {
        if (!dirty_rect_empty(&w->chunks[i].cur)) {
            list[count++] = i;
        }
    }
    GasBlockJob job = {w, list};
    jobs_run(w->jobs, count, gas_blocks_job, &job);
}

void sim_tick(World *w, SimStats *stats) {
    SimStats total = {0};
    PhaseScratch *scratch = get_scratch(w);
    uint8_t stamp = (uint8_t)w->tick;

    // Creatures first (serial, in list order); what they move is woken for this tick.
    creatures_update(w);

    int n = w->chunks_x * w->chunks_y;
    for (int i = 0; i < n; i++) {
        w->chunks[i].cur = w->chunks[i].next;
        dirty_rect_clear(&w->chunks[i].next);
        w->chunks[i].head_dirty = w->chunks[i].head_dirty_next;
        w->chunks[i].head_dirty_next = 0;
        w->chunks[i].heat_active = w->chunks[i].heat_active_next;
        w->chunks[i].heat_active_next = 0;
    }
    // Cells moved into these chunks last tick: they need the heat pass only if temperatures differ
    // within the chunk or against its neighbours (a world at one temperature costs nothing).
    for (int cy = 0; cy < w->chunks_y; cy++) {
        for (int cx = 0; cx < w->chunks_x; cx++) {
            Chunk *ch = world_chunk(w, cx, cy);
            if (!ch->heat_check) {
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
    gas_blocks_update(w, scratch->chunk_list);

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

        jobs_run(w->jobs, jobs, phase_job, scratch->ctx);

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
                if (!dirty_rect_empty(&c->wake[k])) {
                    DirtyRect *r = &c->wake[k];
                    dirty_rect_add(&chunk->next, r->min_x, r->min_y, r->max_x, r->max_y);
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
