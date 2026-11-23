// Movement pass. Rules are documented in docs/SIMULATION.md; keep them in sync.
//
// HOT PATH: everything below runs per cell per tick. No allocation, logging, SDL calls, locks
// or function pointers. Respect MAX_MOVE.

#include "sim/sim.h"

#include "core/rng.h"

typedef struct TickCtx {
    World *w;
    Rng rng;
    uint8_t stamp;
    SimStats stats;
} TickCtx;

// Gravity: liquids gain 1/16 px/tick of fall speed with this probability (x/65536) each tick.
// 0.87/16 px/tick^2 * 60^2 * 0.05 m = 9.8 m/s^2 at 5 cm per pixel. Random rather than on fixed
// ticks: cells released together would otherwise fall as rigid horizontal lines.
#define GRAVITY_INC_P16 57000u
#define FALL_SPEED_MAX  127 // 1/16 px/tick units; ~8 px/tick

// How far a liquid looks sideways for somewhere to fall. Must not exceed MAX_MOVE: rules may only
// read and write within MAX_MOVE of the cell (threading invariant).
#define LIQUID_LOOKAHEAD MAX_MOVE
// Chance per tick (x/65536) that a level surface cell with open space beside it wanders one cell.
#define P_LIQUID_WANDER_16 16384u

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
    world_wake_pair(w, x0, y0, x1, y1);
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
            world_keep_alive(w, x, y); // jammed behind falling liquid: wait, still falling
            return;
        }
        swap_cells(c, x, y, x, target);
        cell_ptr(w, x, target)->aux = (uint8_t)((dir > 0 ? 0x80 : 0) | speed);
        return;
    }

    // Landing: fall speed turns into extra sideways spread this tick, and is reset, so a resting
    // cell never counts as "in transit".
    int spread = g_elem_spread[cell->element] + (speed >> 4);
    spread = PR_MIN(spread, MAX_MOVE);
    cell->aux = (uint8_t)(dir > 0 ? 0x80 : 0);

    // A cell with liquid on top of it is under pressure: it is pushed out sideways into any open
    // space (this is what makes a heap of water spread out instead of slumping like sand), and
    // that takes precedence over sliding diagonally.
    bool pressured = g_elem_behavior[world_elem(w, x, y - 1)] == BEHAVIOR_LIQUID;

    // 2. Slide diagonally down (surface cells first; pressured cells after trying sideways).
    if (!pressured && liquid_try_diagonal(c, x, y, d)) {
        return;
    }

    // 3. Flow sideways, current direction first, then the other. Both kinds of cell look
    //    through their own liquid, up to LIQUID_LOOKAHEAD cells along the row.
    //    Pressured cell (liquid on top of it): moves into open space. Through open space it moves
    //    at most `spread` cells; past its own liquid it moves straight to the first open cell.
    //    So water held up in a stack (poured against a wall, or behind a falling stream) drains
    //    along its whole height, not only from its top; the gap it leaves bubbles up through the
    //    stack, lowering it. Without this, a stream poured beside a wall built a standing tower.
    //    Surface cell: flows only toward a drop (a cell it can fall into); without one it is
    //    level. Looking through its own liquid lets the whole top row of a step flow down to the
    //    next step, not just the step's edge cell (otherwise poured water piles into a stepped
    //    mound). Past its own liquid it moves straight to the drop.
    for (int attempt = 0; attempt < 2; attempt++) {
        int land = 0;
        bool drop = false;
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
            if (pressured) {
                if (passed_own) {
                    land = s;
                    break;
                }
                if (s <= spread) {
                    land = s;
                }
            }
            if (can_displace(w, nx, y + 1, d)) {
                drop = true;
                if (!pressured) {
                    land = passed_own ? s : PR_MIN(s, spread);
                }
                break;
            }
            if (pressured && s >= spread) {
                break;
            }
        }
        if (land > 0 && (drop || pressured)) {
            int cx = x + dir * land;
            swap_cells(c, x, y, cx, y);
            cell_ptr(w, cx, y)->aux = (uint8_t)(dir > 0 ? 0x80 : 0);
            return;
        }
        dir = -dir;
    }

    if (pressured && liquid_try_diagonal(c, x, y, d)) {
        return;
    }

    // 4. No drop in sight, but open space beside: the surface is level to within a step wider
    //    than the lookahead. Wander one cell now and then so such steps still even out (a gentle
    //    surface shimmer). Stay awake while that is possible.
    int wdir = rng_bit(&c->rng) ? 1 : -1;
    for (int k = 0; k < 2; k++, wdir = -wdir) {
        if (can_displace(w, x + wdir, y, d)) {
            if (rng_chance16(&c->rng, P_LIQUID_WANDER_16)) {
                swap_cells(c, x, y, x + wdir, y);
                cell_ptr(w, x + wdir, y)->aux = (uint8_t)(wdir > 0 ? 0x80 : 0);
            } else {
                world_keep_alive(w, x, y);
            }
            return;
        }
    }
    // Enclosed: settle. No wake, so the cell sleeps.
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
        world_keep_alive(w, x, y);
    }
}

static void update_chunk(TickCtx *c, int cx, int cy) {
    World *w = c->w;
    Chunk *chunk = world_chunk(w, cx, cy);
    DirtyRect r = chunk->cur;
    int base_x = cx << CHUNK_SHIFT;
    int base_y = cy << CHUNK_SHIFT;

    c->rng = rng_from_seed(w->seed ^ pr_splitmix64(w->tick) ^
                           (((uint64_t)(uint32_t)cx << 32) | (uint32_t)cy));
    c->stats.chunks_active++;

    // Bottom to top; horizontal direction alternates every tick (no left/right bias).
    bool left_to_right = (w->tick & 1) == 0;
    for (int ly = r.max_y; ly >= r.min_y; ly--) {
        int y = base_y + ly;
        Cell *row = &w->cells[world_index(w, 0, y)];
        for (int i = 0; i <= r.max_x - r.min_x; i++) {
            int x = base_x + (left_to_right ? r.min_x + i : r.max_x - i);
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

void sim_tick(World *w, SimStats *stats) {
    TickCtx c = {0};
    c.w = w;
    c.stamp = (uint8_t)w->tick;

    int n = w->chunks_x * w->chunks_y;
    for (int i = 0; i < n; i++) {
        w->chunks[i].cur = w->chunks[i].next;
        dirty_rect_clear(&w->chunks[i].next);
    }

    // 4-phase checkerboard order. Single-threaded in M1; in M2 the chunks of one phase run in
    // parallel. Using the same order now keeps results identical across the switch.
    for (int i = 0; i < 4; i++) {
        int phase = (int)((i + w->tick) & 3);
        int px = phase & 1, py = phase >> 1;
        for (int cy = py; cy < w->chunks_y; cy += 2) {
            for (int cx = px; cx < w->chunks_x; cx += 2) {
                if (!dirty_rect_empty(&world_chunk(w, cx, cy)->cur)) {
                    update_chunk(&c, cx, cy);
                }
            }
        }
    }

    w->tick++;
    if (stats) {
        *stats = c.stats;
    }
}

void sim_run_ticks(World *w, int n) {
    for (int i = 0; i < n; i++) {
        sim_tick(w, NULL);
    }
}
