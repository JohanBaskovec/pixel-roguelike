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
#include "sim/pressure.h"

#include <string.h>

#define TICK_MAX_REQUESTS 128 // water-pressure requests per chunk per tick; more retry next tick

// Per-job state. Also holds the job's results (wakes, stats) until they are merged.
typedef struct TickCtx {
    World *w;
    Rng rng;
    uint8_t stamp;
    int cx, cy;         // the chunk this job updates
    int base_x, base_y; // its top-left cell
    DirtyRect wake[9];  // next-tick wakes for the 3x3 chunks around it (chunk-local coords)
    uint8_t render[9];  // ... and whether they need re-rendering
    uint8_t liquid[9];  // ... and whether liquid moved there (recompute water pressure)
    SimStats stats;
    int nreq; // water-pressure requests, carried out after the phase
    PressureRequest req[TICK_MAX_REQUESTS];
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

static const int8_t k_dir8[8][2] = {
    {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1},
};

static inline Cell *cell_ptr(World *w, int x, int y) {
    return &w->cells[world_index(w, x, y)];
}

// Swaps two cells (and their temperatures) up to MAX_MOVE apart. Multi-cell moves are done as one
// swap from start to destination: the cells passed over are void (unchanged by the move) or a
// lighter fluid that simply trades places with the mover.
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
    // Both cells changed: wake their 3x3 neighbourhoods (as one rect) and re-render.
    // Still liquid moved (or started to fall): its body must be relabelled for water pressure.
    // Falling liquid passing through does not change any body.
    bool liquid = cell_is_still_liquid(a) || cell_is_still_liquid(b);
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
        return;
    }

    // Landing: fall speed turns into extra sideways spread this tick, and is reset, so a resting
    // cell never counts as "in transit". A landed cell joins a body of still water.
    int spread = g_elem_spread[cell->element] + (speed >> 4);
    spread = PR_MIN(spread, MAX_MOVE);
    cell->aux = (uint8_t)(dir > 0 ? 0x80 : 0);
    if (speed > 0) {
        ctx_wake(c, x, y, x, y, false, true);
    }

    // 2. Water pressure: if the body of still water this cell belongs to has its top surface at
    //    least one row above an open cell above or beside this cell, ask for one cell of water
    //    to be moved from that top surface to the open cell (carried out after the phase, see
    //    pressure.c). This levels pools over any distance, balances U-tubes and makes deep water
    //    push out of openings.
    bool labelled = w->piece[world_index(w, x, y)] != 0;
    int body_top = labelled ? pressure_body_top(w, x, y) : PRESSURE_NO_TOP;
    if (body_top != PRESSURE_NO_TOP) {
        const int q[3][2] = {{x, y - 1}, {x + dir, y}, {x - dir, y}};
        for (int k = 0; k < 3; k++) {
            int qx = q[k][0], qy = q[k][1];
            if (body_top <= qy - 1 && can_displace(w, qx, qy, d)) {
                if (c->nreq < TICK_MAX_REQUESTS) {
                    c->req[c->nreq++] = (PressureRequest){x, y, qx, qy};
                } else {
                    keep_alive(c, x, y); // no room this tick: retry next tick
                }
                return;
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

// True if the gas at (x, y) could still move: a neighbour is void or a different gas, or a
// denser fluid sits above it.
static bool gas_is_mobile(const World *w, int x, int y, ElementId self) {
    for (int i = 0; i < 8; i++) {
        ElementId t = world_elem(w, x + k_dir8[i][0], y + k_dir8[i][1]);
        if (t == ELEM_VOID || (t != self && g_elem_behavior[t] == BEHAVIOR_GAS)) {
            return true;
        }
    }
    ElementId up = world_elem(w, x, y - 1);
    return up != self && elem_is_fluid(up) && g_elem_density_q[up] > g_elem_density_q[self];
}

static void update_gas(TickCtx *c, int x, int y) {
    World *w = c->w;
    ElementId self = cell_ptr(w, x, y)->element;
    uint32_t d = g_elem_density_q[self];

    // 1. Buoyancy: rise through a denser fluid above.
    ElementId up = world_elem(w, x, y - 1);
    if (up != self && elem_is_fluid(up) && g_elem_density_q[up] > d) {
        if (rng_chance16(&c->rng, g_buoy_p[up][self])) {
            swap_cells(c, x, y, x, y - 1);
            return;
        }
    }

    // 2. Random walk. Into void: expansion, several cells at once. Into another gas: mixing.
    int r = (int)rng_range(&c->rng, 8);
    int dx = k_dir8[r][0], dy = k_dir8[r][1];
    ElementId t = world_elem(w, x + dx, y + dy);
    if (t == ELEM_VOID) {
        int cx = x, cy = y;
        int steps = g_elem_spread[self];
        while (steps-- > 0 && world_elem(w, cx + dx, cy + dy) == ELEM_VOID &&
               (dx == 0 || dy == 0 || diagonal_open(w, cx, cy, dx, dy))) {
            cx += dx;
            cy += dy;
        }
        if (cx != x || cy != y) {
            swap_cells(c, x, y, cx, cy);
            return;
        }
    } else if (t != self && g_elem_behavior[t] == BEHAVIOR_GAS &&
               (dx == 0 || dy == 0 || diagonal_open(w, x, y, dx, dy))) {
        uint32_t p = P_GAS_MIX_16;
        uint32_t dt = g_elem_density_q[t];
        // Mixing against stratification (denser gas moving up, lighter moving down) is rarer.
        if ((d > dt && dy < 0) || (d < dt && dy > 0)) {
            p /= 4;
        }
        if (rng_chance16(&c->rng, p)) {
            swap_cells(c, x, y, x + dx, y + dy);
            return;
        }
    }

    // 3. Did not move. Stay awake only if a move is still possible.
    if (gas_is_mobile(w, x, y, self)) {
        keep_alive(c, x, y);
    }
}

static void update_chunk(TickCtx *c) {
    World *w = c->w;
    DirtyRect r = world_chunk(w, c->cx, c->cy)->cur;

    c->rng = rng_from_seed(w->seed ^ pr_splitmix64(w->tick) ^
                           (((uint64_t)(uint32_t)c->cx << 32) | (uint32_t)c->cy));
    c->stats.chunks_active++;

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
                default:
                    break;
            }
        }
    }
}

// Scratch memory for one phase: the chunks to update and one TickCtx per chunk.
typedef struct PhaseScratch {
    int *chunk_list;
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
        size_t bytes = sizeof(PhaseScratch) + (size_t)capacity * (sizeof(int) + sizeof(TickCtx));
        PhaseScratch *s = pr_aligned_alloc_zero(64, bytes + 64);
        PR_ASSERT_ALWAYS(s != NULL);
        s->capacity = capacity;
        s->ctx = (TickCtx *)(((uintptr_t)(s + 1) + 63) & ~(uintptr_t)63);
        s->chunk_list = (int *)(s->ctx + capacity);
        w->sim_scratch = s;
    }
    return w->sim_scratch;
}

void sim_tick(World *w, SimStats *stats) {
    SimStats total = {0};
    PhaseScratch *scratch = get_scratch(w);
    uint8_t stamp = (uint8_t)w->tick;

    int n = w->chunks_x * w->chunks_y;
    for (int i = 0; i < n; i++) {
        w->chunks[i].cur = w->chunks[i].next;
        dirty_rect_clear(&w->chunks[i].next);
        w->chunks[i].head_dirty = w->chunks[i].head_dirty_next;
        w->chunks[i].head_dirty_next = 0;
    }

    // Water pressure: relabel still liquid where it changed and rebuild the bodies. This may add
    // whole chunks to this tick's dirty rects (where a body's top rose).
    pressure_update(w);

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
                c->stamp = stamp;
                c->cx = cx;
                c->cy = cy;
                c->base_x = cx << CHUNK_SHIFT;
                c->base_y = cy << CHUNK_SHIFT;
                for (int k = 0; k < 9; k++) {
                    dirty_rect_clear(&c->wake[k]);
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
            }
            total.cells_visited += c->stats.cells_visited;
            total.cells_moved += c->stats.cells_moved;
            total.chunks_active += c->stats.chunks_active;
        }
        // Water-pressure transfers asked for in this phase, serially, in job order.
        for (int j = 0; j < jobs; j++) {
            TickCtx *c = &scratch->ctx[j];
            total.requests += (uint32_t)c->nreq;
            total.transfers += (uint32_t)pressure_apply(w, c->req, c->nreq);
        }
    }

    w->tick++;
    if (stats) {
        *stats = total;
    }
}

int sim_pressure_depth(const World *w, int x, int y) {
    if (!world_in_bounds(w, x, y) || !w->piece[world_index(w, x, y)]) {
        return -1;
    }
    int top = pressure_body_top(w, x, y);
    return top == PRESSURE_NO_TOP ? -1 : PR_MAX(y - top, 0);
}

void sim_run_ticks(World *w, int n) {
    for (int i = 0; i < n; i++) {
        sim_tick(w, NULL);
    }
}
