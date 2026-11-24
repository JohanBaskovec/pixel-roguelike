#include "sim/path.h"

#include "sim/particle.h"

#include <string.h>

#define INF 0xFFFFu

typedef struct PathField {
    int bx0, by0;         // block coordinates of the window's top-left block
    int bw, bh;           // window size in blocks
    uint64_t built_tick;  // world tick of the last rebuild
    bool valid;           // a field is built and the player's block was open
    uint8_t open[PATH_BLOCKS_X * PATH_BLOCKS_Y];
    uint8_t openf[PATH_BLOCKS_X * PATH_BLOCKS_Y]; // open with every neighbour open: room for a flyer
    uint16_t dist_fly[PATH_BLOCKS_X * PATH_BLOCKS_Y];
    uint16_t dist_walk[PATH_BLOCKS_X * PATH_BLOCKS_Y];
    int32_t queue[PATH_BLOCKS_X * PATH_BLOCKS_Y];
} PathField;

static inline bool in_win(const PathField *f, int c, int r) {
    return c >= 0 && r >= 0 && c < f->bw && r < f->bh;
}

static inline bool is_open(const PathField *f, int c, int r) {
    return in_win(f, c, r) && f->open[r * f->bw + c];
}

static inline bool is_openf(const PathField *f, int c, int r) {
    return in_win(f, c, r) && f->openf[r * f->bw + c];
}

// Can a body see through this cell? (Same as enemy.c's line of sight.)
static bool passable(const World *w, int x, int y) {
    ElementId e = world_elem(w, x, y);
    uint8_t b = g_elem_behavior[e];
    return e == ELEM_BODY || b == BEHAVIOR_NONE || b == BEHAVIOR_GAS || b == BEHAVIOR_LIQUID;
}

void path_free(World *w) {
    pr_aligned_free(w->paths);
    w->paths = NULL;
}

bool path_update(World *w, int px, int py) {
    PathField *f = w->paths;
    if (!f) {
        f = pr_aligned_alloc_zero(64, sizeof(PathField));
        if (!f) {
            return false;
        }
        w->paths = f;
        f->built_tick = ~0ull;
    }
    if (f->built_tick != ~0ull && w->tick - f->built_tick < PATH_TICKS && w->tick >= f->built_tick) {
        return f->valid;
    }
    f->built_tick = w->tick;
    f->valid = false;
    // The window: twice the screen around the player, in whole blocks, inside the world.
    int bw = PR_CLAMP(2 * w->view_w / PATH_BLOCK + 2, 8, PATH_BLOCKS_X);
    int bh = PR_CLAMP(2 * w->view_h / PATH_BLOCK + 2, 8, PATH_BLOCKS_Y);
    int wbx = w->width / PATH_BLOCK, wby = w->height / PATH_BLOCK;
    bw = PR_MIN(bw, wbx);
    bh = PR_MIN(bh, wby);
    f->bw = bw;
    f->bh = bh;
    f->bx0 = PR_CLAMP(px / PATH_BLOCK - bw / 2, 0, wbx - bw);
    f->by0 = PR_CLAMP(py / PATH_BLOCK - bh / 2, 0, wby - bh);
    for (int r = 0; r < bh; r++) {
        for (int c = 0; c < bw; c++) {
            int x = (f->bx0 + c) * PATH_BLOCK + PATH_BLOCK / 2;
            int y = (f->by0 + r) * PATH_BLOCK + PATH_BLOCK / 2;
            f->open[r * bw + c] = passable(w, x, y);
            f->dist_fly[r * bw + c] = INF;
            f->dist_walk[r * bw + c] = INF;
        }
    }
    // A flyer is wider than a block (11 cells): it keeps a block clear of walls.
    for (int r = 0; r < bh; r++) {
        for (int c = 0; c < bw; c++) {
            bool all = true;
            for (int k = 0; k < 9 && all; k++) {
                all = is_open(f, c + k % 3 - 1, r + k / 3 - 1);
            }
            f->openf[r * bw + c] = all;
        }
    }
    // The targets: the player's block, or an open one next to it (for each kind of monster).
    int tc = px / PATH_BLOCK - f->bx0, tr = py / PATH_BLOCK - f->by0, best = -1, bestf = -1;
    for (int k = 0; k < 9; k++) {
        int c = tc + (k == 0 ? 0 : (k - 1) % 3 - 1), r = tr + (k == 0 ? 0 : (k - 1) / 3 - 1);
        if (best < 0 && is_open(f, c, r)) {
            best = r * bw + c;
        }
        if (bestf < 0 && is_openf(f, c, r)) {
            bestf = r * bw + c;
        }
    }
    if (best < 0) {
        return false;
    }
    f->valid = true;

    // Flyers: any move to an open side or vertical neighbour.
    int head = 0, tail = 0;
    if (bestf >= 0) {
        f->queue[tail++] = bestf;
        f->dist_fly[bestf] = 0;
    }
    while (head < tail) {
        int i = f->queue[head++], c = i % bw, r = i / bw;
        static const int8_t d4[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (int k = 0; k < 4; k++) {
            int nc = c + d4[k][0], nr = r + d4[k][1];
            if (is_openf(f, nc, nr) && f->dist_fly[nr * bw + nc] == INF) {
                f->dist_fly[nr * bw + nc] = (uint16_t)(f->dist_fly[i] + 1);
                f->queue[tail++] = nr * bw + nc;
            }
        }
    }

    // Walkers, searching backwards from the target over the moves a walker can make: to a side
    // neighbour, down to the block below (a fall), and up one block onto a ledge (diagonally, past
    // a solid block beside it): who can reach block m is whoever stands next to it, above it, or
    // diagonally below it beside a ledge.
    head = tail = 0;
    f->queue[tail++] = best;
    f->dist_walk[best] = 0;
    while (head < tail) {
        int i = f->queue[head++], c = i % bw, r = i / bw;
        uint16_t nd = (uint16_t)(f->dist_walk[i] + 1);
        int pc[6], pr[6], n = 0;
        pc[n] = c - 1, pr[n++] = r; // from the sides
        pc[n] = c + 1, pr[n++] = r;
        pc[n] = c, pr[n++] = r - 1; // from above (it falls into m)
        for (int s = -1; s <= 1; s += 2) {
            // From b = (c - s, r + 1) up onto m = (c, r): the block beside b at m's height is open
            // (headroom), the one beside m's floor is solid (the ledge).
            if (is_open(f, c - s, r) && !is_open(f, c, r + 1)) {
                pc[n] = c - s, pr[n++] = r + 1;
            }
        }
        for (int k = 0; k < n; k++) {
            if (is_open(f, pc[k], pr[k]) && f->dist_walk[pr[k] * bw + pc[k]] == INF) {
                f->dist_walk[pr[k] * bw + pc[k]] = nd;
                f->queue[tail++] = pr[k] * bw + pc[k];
            }
        }
    }
    return true;
}

// The window block holding cell (x, y), or an open block next to it (above first); -1 if none.
static int node_of(const PathField *f, int x, int y, bool fly) {
    int c = x / PATH_BLOCK - f->bx0, r = y / PATH_BLOCK - f->by0;
    static const int8_t order[9][2] = {{0, 0},  {0, -1}, {-1, 0}, {1, 0}, {0, 1},
                                       {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int k = 0; k < 9; k++) {
        if (fly ? is_openf(f, c + order[k][0], r + order[k][1])
                : is_open(f, c + order[k][0], r + order[k][1])) {
            return (r + order[k][1]) * f->bw + c + order[k][0];
        }
    }
    return -1;
}

bool path_flyer_step(const World *w, int cx, int cy, float *tx, float *ty) {
    const PathField *f = w->paths;
    if (!f || !f->valid) {
        return false;
    }
    int n = node_of(f, cx, cy, true);
    if (n < 0 || f->dist_fly[n] == INF) {
        return false;
    }
    int c = n % f->bw, r = n / f->bw, bc = -1, br = -1;
    uint16_t best = f->dist_fly[n];
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int nc = c + dc, nr = r + dr;
            if ((dc == 0 && dr == 0) || !is_openf(f, nc, nr)) {
                continue;
            }
            // A diagonal needs both blocks it cuts across (a body has size).
            if (dc != 0 && dr != 0 && (!is_openf(f, c + dc, r) || !is_openf(f, c, r + dr))) {
                continue;
            }
            if (f->dist_fly[nr * f->bw + nc] < best) {
                best = f->dist_fly[nr * f->bw + nc];
                bc = nc;
                br = nr;
            }
        }
    }
    if (bc < 0) {
        return false; // at the target block
    }
    *tx = (float)((f->bx0 + bc) * PATH_BLOCK + PATH_BLOCK / 2) + 0.5f;
    *ty = (float)((f->by0 + br) * PATH_BLOCK + PATH_BLOCK / 2) + 0.5f;
    return true;
}

bool path_walker_step(const World *w, int cx, int cy, int *walk, int *jump) {
    const PathField *f = w->paths;
    if (!f || !f->valid) {
        return false;
    }
    int n = node_of(f, cx, cy, false);
    if (n < 0 || f->dist_walk[n] == INF) {
        return false;
    }
    int c = n % f->bw, r = n / f->bw;
    uint16_t here = f->dist_walk[n], best = here;
    int bwalk = 0, bjump = 0;
    #define DIST(cc, rr) (is_open(f, (cc), (rr)) ? f->dist_walk[(rr) * f->bw + (cc)] : INF)
    for (int s = -1; s <= 1; s += 2) {
        uint16_t d = DIST(c + s, r);
        if (d < best) {
            best = d;
            bwalk = s;
            bjump = 0;
        }
        // Up onto a ledge: the block beside is solid, the one above it open.
        d = DIST(c + s, r - 1);
        if (d < best && is_open(f, c, r - 1) && !is_open(f, c + s, r)) {
            best = d;
            bwalk = s;
            bjump = 1;
        }
    }
    uint16_t down = DIST(c, r + 1);
    if (down < best) {
        best = down;
        bwalk = 0;
        bjump = 0;
        // Falling into the block below: first centre the body over it (at the edge of a hole it
        // would stand on the rim).
        int off = (f->bx0 + c) * PATH_BLOCK + PATH_BLOCK / 2 - cx;
        bwalk = off > 0 ? 1 : (off < 0 ? -1 : 0);
    }
    #undef DIST
    if (best >= here) {
        return false; // already at the lowest point it can reach
    }
    *walk = bwalk;
    *jump = bjump;
    return true;
}
