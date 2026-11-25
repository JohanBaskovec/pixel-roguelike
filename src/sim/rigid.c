#include "sim/rigid.h"

#include "core/rng.h"
#include "sim/particle.h"

#include <math.h>
#include <string.h>

#define RESTITUTION      0.25f // of an impact faster than BOUNCE_MIN, cells per tick
#define BOUNCE_MIN       0.3f
#define FRICTION         0.6f
#define RESTING_DAMPING  0.9f
#define SPIN_DAMPING     0.98f // per tick while touching something
#define MAX_SPEED        12.0f // cells per tick
#define MAX_SUBSTEPS     12
#define SLEEP_SPEED      0.07f // cells per tick, for the fastest pixel
#define SLEEP_TICKS      30
#define SOFT_CELLS       2     // cells a body may overlap, plus a 32nd of its pixels
#define WAKE_CHECK_TICKS 8     // a sleeping body checks its support this often

static inline bool is_wood(ElementId e) {
    return rigid_material(e);
}

// ---- Small set of grid indices (scratch, serial use) ----

#define SET_SIZE 4096 // a power of two, over twice RIGID_MAX_CELLS

typedef struct IdxSet {
    int32_t key[SET_SIZE]; // -1: empty
    int16_t val[SET_SIZE];
} IdxSet;

static IdxSet g_own, g_old, g_new;

static void set_clear(IdxSet *s) {
    memset(s->key, 0xFF, sizeof(s->key));
}

static void set_put(IdxSet *s, int32_t k, int16_t v) {
    uint32_t h = ((uint32_t)k * 2654435761u) & (SET_SIZE - 1);
    while (s->key[h] != -1 && s->key[h] != k) {
        h = (h + 1) & (SET_SIZE - 1);
    }
    s->key[h] = k;
    s->val[h] = v;
}

static int set_get(const IdxSet *s, int32_t k) {
    uint32_t h = ((uint32_t)k * 2654435761u) & (SET_SIZE - 1);
    while (s->key[h] != -1) {
        if (s->key[h] == k) {
            return s->val[h];
        }
        h = (h + 1) & (SET_SIZE - 1);
    }
    return -1;
}

// ---- Mass properties ----

static void props(Rigid *r) {
    int n = 0;
    float sx = 0.0f, sy = 0.0f;
    for (int j = 0; j < r->h; j++) {
        for (int i = 0; i < r->w; i++) {
            if (r->pix[j * r->w + i].cell.element != ELEM_VOID) {
                n++;
                sx += (float)i + 0.5f;
                sy += (float)j + 0.5f;
            }
        }
    }
    r->pixels = n;
    if (n == 0) {
        return;
    }
    r->cx = sx / (float)n;
    r->cy = sy / (float)n;
    float inertia = (float)n / 6.0f, far2 = 0.0f;
    for (int j = 0; j < r->h; j++) {
        for (int i = 0; i < r->w; i++) {
            if (r->pix[j * r->w + i].cell.element != ELEM_VOID) {
                float dx = (float)i + 0.5f - r->cx, dy = (float)j + 0.5f - r->cy;
                inertia += dx * dx + dy * dy;
                far2 = fmaxf(far2, dx * dx + dy * dy);
            }
        }
    }
    r->inertia = inertia;
    r->radius = sqrtf(far2) + 0.8f;
}

// ---- Rasterising a pose ----

typedef struct RasterCell {
    int32_t idx;
    uint16_t pix;
} RasterCell;

// The world cells whose centres fall on a pixel of r at pose (x, y, ang): the inverse mapping, so a
// rotated body has no holes. Returns the count, or -1 if a pixel would be outside the world (or
// the body fills more cells than it can).
static int raster(const World *w, const Rigid *r, float x, float y, float ang, RasterCell *out) {
    float c = cosf(ang), s = sinf(ang), reach = r->radius + 1.0f;
    int x0 = (int)floorf(x - reach), x1 = (int)floorf(x + reach);
    int y0 = (int)floorf(y - reach), y1 = (int)floorf(y + reach);
    int n = 0;
    for (int Y = y0; Y <= y1; Y++) {
        for (int X = x0; X <= x1; X++) {
            float px = (float)X + 0.5f - x, py = (float)Y + 0.5f - y;
            int i = (int)floorf(c * px + s * py + r->cx);
            int j = (int)floorf(-s * px + c * py + r->cy);
            if (i < 0 || j < 0 || i >= r->w || j >= r->h ||
                r->pix[j * r->w + i].cell.element == ELEM_VOID) {
                continue;
            }
            if (!world_in_bounds(w, X, Y) || n >= RIGID_MAX_CELLS) {
                return -1;
            }
            out[n].idx = (int32_t)world_index(w, X, Y);
            out[n].pix = (uint16_t)(j * r->w + i);
            n++;
        }
    }
    return n;
}

// May the body be where cell idx is? Void and gas (which it displaces), falling blood (a
// droplet, which trades places like gas: a bleeding corpse must not be held up by its own drops),
// and its own cells. Anything else is solid to it, still blood included: a corpse floats on a
// pool of blood as a log floats on water. (When moving bodies took all blood as free but a
// sleeping body was held up by it, a corpse in a pool sank, was held, woke and sank again forever,
// trading ~20 cells of blood with gas every tick and burying its own drops: D-051.)
static bool g_support_check; // checking whether a sleeping body is held up: any blood holds it up

static inline bool free_for(const World *w, int32_t idx) {
    Cell c = w->cells[idx];
    uint8_t b = g_elem_behavior[c.element];
    return (b == BEHAVIOR_NONE || b == BEHAVIOR_GAS) ||
           (c.element == ELEM_BLOOD && (c.aux & 0x7F) != 0 && !g_support_check) ||
           set_get(&g_own, idx) >= 0;
}

static void own_build(const Rigid *r) {
    set_clear(&g_own);
    for (int k = 0; k < r->ncells; k++) {
        set_put(&g_own, r->cell_idx[k], (int16_t)k);
    }
}

// ---- Putting the pixels in the grid ----

static void put_pixel(World *w, int32_t idx, const RigidPixel *p) {
    Cell c = p->cell;
    c.stamp = (uint8_t)(w->tick - 1); // simulated this tick
    w->cells[idx] = c;
    w->temp[idx] = p->temp;
    gas_mix_clear(&w->gas[idx]);
}

static void move_matter(World *w, int32_t from, int32_t to) {
    w->cells[to] = w->cells[from];
    w->cells[to].stamp = (uint8_t)(w->tick - 1);
    w->temp[to] = w->temp[from];
    w->gas[to] = w->gas[from];
}

static void clear_cell(World *w, int32_t idx) {
    w->cells[idx] = (Cell){ELEM_VOID, (uint8_t)(w->tick - 1), w->cells[idx].shade, 0};
    w->temp[idx] = 0.0f;
    gas_mix_clear(&w->gas[idx]);
}

// A cell that belongs to no body any more (a sliver cut off, too small to be one) crumbles: wood
// becomes sawdust, a powder, which falls and piles up like sand; flesh becomes a drop of blood.
static void crumble(World *w, int32_t idx) {
    Cell c = w->cells[idx];
    ElementId to = c.element == ELEM_WALKER_CORPSE || c.element == ELEM_FLYER_CORPSE ? ELEM_BLOOD
                                                                                      : ELEM_SAWDUST;
    w->cells[idx] = (Cell){to, (uint8_t)(w->tick - 1), c.shade, 0};
}

static void note(World *w, int32_t idx) {
    world_note_moved(w, (int)(idx % w->width), (int)(idx / w->width));
}

// Makes the grid show the body at the raster nc[0..n): cells it leaves trade places with cells it
// enters (what was in them, gas, goes to the cell it leaves), so nothing is lost.
static void commit(World *w, Rigid *r, const RasterCell *nc, int n) {
    set_clear(&g_old);
    set_clear(&g_new);
    for (int k = 0; k < r->ncells; k++) {
        set_put(&g_old, r->cell_idx[k], (int16_t)k);
    }
    for (int k = 0; k < n; k++) {
        set_put(&g_new, nc[k].idx, (int16_t)k);
    }
    static int32_t vacated[RIGID_MAX_CELLS], entered[RIGID_MAX_CELLS];
    int nv = 0, ne = 0;
    for (int k = 0; k < r->ncells; k++) {
        if (set_get(&g_new, r->cell_idx[k]) < 0) {
            vacated[nv++] = r->cell_idx[k];
        }
    }
    for (int k = 0; k < n; k++) {
        if (set_get(&g_old, nc[k].idx) < 0) {
            entered[ne++] = nc[k].idx;
        }
    }
    int lo_x = w->width, lo_y = w->height, hi_x = 0, hi_y = 0;
    GasMix pool;
    gas_mix_clear(&pool);
    float pool_energy = 0.0f;
    // Pair each entered cell with the nearest vacated one that is left.
    for (int a = 0; a < ne; a++) {
        int ax = (int)(entered[a] % w->width), ay = (int)(entered[a] / w->width);
        int best = -1, best_d = 1 << 30;
        for (int v = 0; v < nv; v++) {
            if (vacated[v] < 0) {
                continue;
            }
            int dx = (int)(vacated[v] % w->width) - ax, dy = (int)(vacated[v] / w->width) - ay;
            int d = dx * dx + dy * dy;
            if (d < best_d) {
                best_d = d;
                best = v;
            }
        }
        if (best >= 0) {
            move_matter(w, entered[a], vacated[best]); // the displaced gas moves into the gap
            note(w, vacated[best]);
            vacated[best] = -1;
        } else if (gas_total(&w->gas[entered[a]]) > 0.0f) {
            // No gap left to put it in: it is gathered, and spread over the gas around the body.
            float cap = gas_heat_cap(&w->gas[entered[a]]);
            gas_mix_add(&pool, &w->gas[entered[a]], 1.0f);
            pool_energy += cap * w->temp[entered[a]];
        }
    }
    for (int v = 0; v < nv; v++) {
        if (vacated[v] >= 0) {
            clear_cell(w, vacated[v]); // a gap nothing fills: void, which the gas flows into
            note(w, vacated[v]);
        }
    }
    for (int k = 0; k < n; k++) {
        put_pixel(w, nc[k].idx, &r->pix[nc[k].pix]);
        note(w, nc[k].idx);
        int x = (int)(nc[k].idx % w->width), y = (int)(nc[k].idx / w->width);
        lo_x = PR_MIN(lo_x, x);
        hi_x = PR_MAX(hi_x, x);
        lo_y = PR_MIN(lo_y, y);
        hi_y = PR_MAX(hi_y, y);
    }
    for (int k = 0; k < r->ncells; k++) {
        int x = (int)(r->cell_idx[k] % w->width), y = (int)(r->cell_idx[k] / w->width);
        lo_x = PR_MIN(lo_x, x);
        hi_x = PR_MAX(hi_x, x);
        lo_y = PR_MIN(lo_y, y);
        hi_y = PR_MAX(hi_y, y);
    }
    for (int k = 0; k < n; k++) {
        r->cell_idx[k] = nc[k].idx;
        r->cell_pix[k] = nc[k].pix;
    }
    r->ncells = n;
    if (gas_total(&pool) > 0.0f) {
        // The gas the body pushed out: into the gas cells next to it (not those it now fills).
        static int32_t ring[RIGID_MAX_CELLS];
        int nr = 0;
        for (int k = 0; k < n && nr < RIGID_MAX_CELLS; k++) {
            int x = (int)(nc[k].idx % w->width), y = (int)(nc[k].idx / w->width);
            static const int8_t d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int q = 0; q < 4 && nr < RIGID_MAX_CELLS; q++) {
                int qx = x + d4[q][0], qy = y + d4[q][1];
                if (world_elem(w, qx, qy) != ELEM_GAS) {
                    continue;
                }
                int32_t qi = (int32_t)world_index(w, qx, qy);
                bool dup = false;
                for (int u = 0; u < nr && !dup; u++) {
                    dup = ring[u] == qi;
                }
                if (!dup) {
                    ring[nr++] = qi;
                }
            }
        }
        if (nr > 0) {
            world_gas_spread_add(w, ring, nr, &pool, pool_energy / gas_heat_cap(&pool));
        }
    }
    world_wake_rect(w, lo_x - 2, lo_y - 2, hi_x + 2, hi_y + 2);
    world_wake_heat(w, lo_x - 1, lo_y - 1, hi_x + 1, hi_y + 1);
}

// ---- Creating bodies ----

static int new_slot(World *w) {
    for (int i = 0; i < w->rigid_count; i++) {
        if (!w->rigids[i].active) {
            return i;
        }
    }
    return w->rigid_count < RIGID_MAX ? w->rigid_count++ : -1;
}

int rigid_spawn_box(World *w, float cx, float cy, int bw, int bh, float angle) {
    if (bw < 1 || bh < 1 || bw > RIGID_MAX_DIM || bh > RIGID_MAX_DIM || bw * bh > RIGID_MAX_CELLS / 2) {
        return -1;
    }
    int slot = new_slot(w);
    if (slot < 0) {
        return -1;
    }
    Rigid *r = &w->rigids[slot];
    memset(r, 0, sizeof(*r));
    r->w = (int16_t)bw;
    r->h = (int16_t)bh;
    uint64_t h = pr_splitmix64(w->seed ^ 0x2160ull ^ (w->tick << 8) ^ (uint64_t)slot);
    for (int k = 0; k < bw * bh; k++) {
        h = pr_splitmix64(h);
        r->pix[k].cell = (Cell){ELEM_WOOD, 0, (uint8_t)(h >> 20), 0};
        r->pix[k].temp = g_elem_defs[ELEM_WOOD].spawn_temp;
    }
    props(r);
    r->x = cx;
    r->y = cy;
    r->angle = angle;
    static RasterCell rc[RIGID_MAX_CELLS];
    int n = raster(w, r, cx, cy, angle, rc);
    bool ok = n > 0;
    for (int k = 0; k < n && ok; k++) {
        uint8_t b = g_elem_behavior[w->cells[rc[k].idx].element];
        ok = b == BEHAVIOR_NONE || b == BEHAVIOR_GAS;
    }
    if (!ok) {
        r->active = 0;
        if (slot == w->rigid_count - 1) {
            w->rigid_count--;
        }
        return -1;
    }
    r->active = 1;
    r->ncells = 0;
    commit(w, r, rc, n);
    return slot;
}

int rigid_at(const World *w, int x, int y) {
    if (!world_in_bounds(w, x, y)) {
        return -1;
    }
    int32_t idx = (int32_t)world_index(w, x, y);
    for (int i = 0; i < w->rigid_count; i++) {
        const Rigid *r = &w->rigids[i];
        if (!r->active || fabsf((float)x - r->x) > r->radius + 2.0f ||
            fabsf((float)y - r->y) > r->radius + 2.0f) {
            continue;
        }
        for (int k = 0; k < r->ncells; k++) {
            if (r->cell_idx[k] == idx) {
                return i;
            }
        }
    }
    return -1;
}

// ---- Impulses ----

// An impulse (Jx, Jy), in pixel masses x cells/tick, at the world point (px, py).
static void impulse(Rigid *r, float px, float py, float jx, float jy) {
    float rx = px - r->x, ry = py - r->y;
    r->vx += jx / (float)r->pixels;
    r->vy += jy / (float)r->pixels;
    r->omega += (rx * jy - ry * jx) / r->inertia;
}

void rigid_hit(World *w, int x, int y, float ux, float uy, float momentum) {
    int i = rigid_at(w, x, y);
    if (i < 0) {
        return;
    }
    Rigid *r = &w->rigids[i];
    // momentum / (mass of a wood cell) in cells/tick, as pixel masses x cells/tick.
    float j = momentum / g_elem_mass[ELEM_WOOD] / CELL_SIZE_M * TICK_SECONDS;
    impulse(r, (float)x + 0.5f, (float)y + 0.5f, ux * j, uy * j);
    r->asleep = 0;
    r->rest_ticks = 0;
}

// ---- Cutting: removed pixels split the body ----

// Splits body ri (some of its pixels were removed) into connected pieces: the biggest stays in
// the slot, the others become new bodies with the velocity of their own part; pieces under
// RIGID_MIN_PIECE pixels crumble to sawdust. The pose of every piece keeps its pixels where they are.
static void restructure(World *w, int ri) {
    Rigid *r = &w->rigids[ri];
    static uint8_t label[RIGID_MAX_DIM * RIGID_MAX_DIM];
    static int16_t stack[RIGID_MAX_DIM * RIGID_MAX_DIM];
    static int size[256];
    memset(label, 0, (size_t)(r->w * r->h));
    memset(size, 0, sizeof(size));
    int comps = 0;
    for (int s = 0; s < r->w * r->h; s++) {
        if (r->pix[s].cell.element == ELEM_VOID || label[s] != 0 || comps >= 255) {
            continue;
        }
        comps++;
        int top = 0;
        stack[top++] = (int16_t)s;
        label[s] = (uint8_t)comps;
        while (top > 0) {
            int p = stack[--top], i = p % r->w, j = p / r->w;
            size[comps]++;
            static const int8_t d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int k = 0; k < 4; k++) {
                int ni = i + d4[k][0], nj = j + d4[k][1];
                if (ni < 0 || nj < 0 || ni >= r->w || nj >= r->h) {
                    continue;
                }
                int q = nj * r->w + ni;
                if (r->pix[q].cell.element != ELEM_VOID && label[q] == 0) {
                    label[q] = (uint8_t)comps;
                    stack[top++] = (int16_t)q;
                }
            }
        }
    }
    // Cells of the old stamp whose pixel is gone (or in a piece that vanishes) are cleared.
    // Snapshot the parent before changing it.
    static int32_t old_idx[RIGID_MAX_CELLS];
    static uint16_t old_pix[RIGID_MAX_CELLS];
    int old_n = r->ncells;
    memcpy(old_idx, r->cell_idx, sizeof(int32_t) * (size_t)old_n);
    memcpy(old_pix, r->cell_pix, sizeof(uint16_t) * (size_t)old_n);
    float px = r->x, py = r->y, pang = r->angle, pvx = r->vx, pvy = r->vy, pom = r->omega;
    float pcx = r->cx, pcy = r->cy;
    float bleed_total = r->bleed;
    int total_px = 0;
    for (int k = 1; k <= comps; k++) {
        total_px += size[k];
    }
    int pw = r->w, ph = r->h;
    float c = cosf(pang), s = sinf(pang);
    // Biggest piece.
    int keep = 0;
    for (int k = 1; k <= comps; k++) {
        if (size[k] > size[keep]) {
            keep = k;
        }
    }
    if (comps == 0 || size[keep] < RIGID_MIN_PIECE) {
        for (int k = 0; k < old_n; k++) {
            if (is_wood(w->cells[old_idx[k]].element)) {
                crumble(w, old_idx[k]);
                note(w, old_idx[k]);
            }
        }
        r->active = 0;
        if (ri == w->rigid_count - 1) {
            w->rigid_count--;
        }
        world_wake_rect(w, (int)px - 40, (int)py - 40, (int)px + 40, (int)py + 40);
        return;
    }
    // Pieces to make: copy their pixels out of the parent's bitmap before it is trimmed.
    typedef struct Piece {
        int label, bx0, by0, bw, bh, slot;
        float cx, cy; // centre of mass in the parent's bitmap
    } Piece;
    Piece pieces[RIGID_MAX];
    int np = 0;
    if (comps > 1) {
        world_sound(w, SOUND_SPLIT, (int)px, (int)py, 0.7f);
    }
    for (int k = 1; k <= comps && np < RIGID_MAX; k++) {
        if (k == keep || size[k] < RIGID_MIN_PIECE) {
            continue;
        }
        int x0 = pw, y0 = ph, x1 = -1, y1 = -1;
        float sx = 0.0f, sy = 0.0f;
        for (int p = 0; p < pw * ph; p++) {
            if (label[p] == k) {
                int i = p % pw, j = p / pw;
                x0 = PR_MIN(x0, i);
                x1 = PR_MAX(x1, i);
                y0 = PR_MIN(y0, j);
                y1 = PR_MAX(y1, j);
                sx += (float)i + 0.5f;
                sy += (float)j + 0.5f;
            }
        }
        int slot = new_slot(w);
        if (slot < 0) {
            continue; // no room: this piece is lost
        }
        // new_slot may have grown rigid_count; `r` stays valid (the array is fixed).
        Rigid *nr = &w->rigids[slot];
        memset(nr, 0, sizeof(*nr));
        nr->active = 1;
        nr->w = (int16_t)(x1 - x0 + 1);
        nr->h = (int16_t)(y1 - y0 + 1);
        for (int p = 0; p < pw * ph; p++) {
            if (label[p] == k) {
                int i = p % pw - x0, j = p / pw - y0;
                nr->pix[j * nr->w + i] = r->pix[p];
            }
        }
        props(nr);
        float dx = sx / (float)size[k] - pcx, dy = sy / (float)size[k] - pcy; // in the old frame
        float wx = c * dx - s * dy, wy = s * dx + c * dy;                     // in the world
        nr->x = px + wx;
        nr->y = py + wy;
        nr->angle = pang;
        nr->vx = pvx - pom * wy; // the velocity of that point of the old body
        nr->vy = pvy + pom * wx;
        nr->omega = pom;
        nr->bleed = bleed_total * (float)size[k] / (float)PR_MAX(total_px, 1); // its share
        pieces[np++] = (Piece){k, x0, y0, nr->w, nr->h, slot, 0.0f, 0.0f};
        // Its cells.
        for (int q = 0; q < old_n; q++) {
            if (label[old_pix[q]] == k) {
                int i = old_pix[q] % pw - x0, j = old_pix[q] / pw - y0;
                nr->cell_idx[nr->ncells] = old_idx[q];
                nr->cell_pix[nr->ncells++] = (uint16_t)(j * nr->w + i);
            }
        }
    }
    // The parent keeps its biggest piece.
    {
        float sx = 0.0f, sy = 0.0f;
        for (int p = 0; p < pw * ph; p++) {
            if (label[p] != keep && r->pix[p].cell.element != ELEM_VOID) {
                r->pix[p].cell.element = ELEM_VOID;
            } else if (label[p] == keep) {
                sx += (float)(p % pw) + 0.5f;
                sy += (float)(p / pw) + 0.5f;
            }
        }
        props(r); // sets the new centre of mass (cx, cy)
        float dx = r->cx - pcx, dy = r->cy - pcy;
        float wx = c * dx - s * dy, wy = s * dx + c * dy;
        r->x = px + wx;
        r->y = py + wy;
        r->vx = pvx - pom * wy;
        r->vy = pvy + pom * wx;
        r->bleed = bleed_total * (float)size[keep] / (float)PR_MAX(total_px, 1);
        int n = 0;
        for (int q = 0; q < old_n; q++) {
            if (label[old_pix[q]] == keep) {
                r->cell_idx[n] = old_idx[q];
                r->cell_pix[n++] = old_pix[q];
            }
        }
        r->ncells = n;
        r->asleep = 0;
        r->rest_ticks = 0;
        (void)sx;
        (void)sy;
    }
    // A piece none of whose pixels is drawn (a sliver the rotated grid does not sample) is gone.
    for (int k = 0; k < np; k++) {
        Rigid *nr = &w->rigids[pieces[k].slot];
        if (nr->ncells == 0) {
            nr->active = 0;
            if (pieces[k].slot == w->rigid_count - 1) {
                w->rigid_count--;
            }
            pieces[k].label = -1;
        }
    }
    if (r->ncells == 0) {
        r->active = 0;
        if (ri == w->rigid_count - 1) {
            w->rigid_count--;
        }
    }
    // Stamp cells of pieces that were not made (too small, or no slot) are cleared.
    for (int q = 0; q < old_n; q++) {
        uint8_t lb = label[old_pix[q]];
        bool placed = lb == keep && r->active;
        for (int k = 0; k < np && !placed; k++) {
            placed = pieces[k].label == lb;
        }
        if (!placed && is_wood(w->cells[old_idx[q]].element)) {
            crumble(w, old_idx[q]); // a sliver (or a piece with no slot): sawdust
            note(w, old_idx[q]);
        }
    }
    world_wake_rect(w, (int)px - 40, (int)py - 40, (int)px + 40, (int)py + 40);
}

// Reads the pixels' state back from the grid (heat, fire), and removes the pixels whose cells are
// not wood any more (shot, erased, burnt away). Returns true if any was removed.
static bool readback(World *w, Rigid *r) {
    bool lost = false;
    for (int k = 0; k < r->ncells; k++) {
        int32_t idx = r->cell_idx[k];
        RigidPixel *p = &r->pix[r->cell_pix[k]];
        if (p->cell.element == ELEM_VOID) {
            continue; // already removed through another cell
        }
        if (is_wood(w->cells[idx].element)) {
            p->cell = w->cells[idx];
            p->temp = w->temp[idx];
        } else {
            p->cell.element = ELEM_VOID;
            lost = true;
        }
    }
    return lost;
}

// ---- Dynamics ----

// A collision of the body at pose (x, y, ang): the free-space normal and contact point of the
// solid cells it would overlap. Returns the number of overlapping cells (0: free), -1 if the
// pose cannot be drawn (outside the world).
static int collide(const World *w, const Rigid *r, float x, float y, float ang, float *nx_out,
                   float *ny_out, float *cx_out, float *cy_out) {
    static RasterCell rc[RIGID_MAX_CELLS];
    int n = raster(w, r, x, y, ang, rc);
    if (n < 0) {
        return -1;
    }
    int hits = 0;
    float sx = 0.0f, sy = 0.0f;
    for (int k = 0; k < n; k++) {
        if (free_for(w, rc[k].idx)) {
            continue;
        }
        hits++;
        int X = (int)(rc[k].idx % w->width), Y = (int)(rc[k].idx / w->width);
        sx += (float)X + 0.5f;
        sy += (float)Y + 0.5f;
    }
    if (hits > 0) {
        *cx_out = sx / (float)hits;
        *cy_out = sy / (float)hits;
        *nx_out = *ny_out = 0.0f; // the caller works the normal out (contact_normal)
    }
    return hits;
}

// The normal of the contact of the body at pose (x, y, ang), which overlaps something: the
// direction of the smallest shift that frees it (axes first), trying 0.5, 1 and 2 cells. The body
// is then pressed against that surface along that direction.
static void contact_normal(const World *w, const Rigid *r, float x, float y, float ang, float *nx,
                           float *ny) {
    static const float k_dir[8][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0},
                                      {-0.7071f, -0.7071f}, {0.7071f, -0.7071f},
                                      {-0.7071f, 0.7071f}, {0.7071f, 0.7071f}};
    static const float k_dist[4] = {0.5f, 1.0f, 2.0f, 4.0f};
    float ox, oy, ocx, ocy;
    for (int d = 0; d < 4; d++) {
        for (int k = 0; k < 8; k++) {
            if (collide(w, r, x + k_dir[k][0] * k_dist[d], y + k_dir[k][1] * k_dist[d], ang, &ox,
                        &oy, &ocx, &ocy) == 0) {
                *nx = k_dir[k][0];
                *ny = k_dir[k][1];
                return;
            }
        }
    }
    float len = sqrtf(r->vx * r->vx + r->vy * r->vy);
    *nx = len > 1e-6f ? -r->vx / len : 0.0f;
    *ny = len > 1e-6f ? -r->vy / len : -1.0f;
}

// The impulse of a collision at contact point (px, py) with normal (nx, ny) (out of the solid).
static void collision_impulse(Rigid *r, float px, float py, float nx, float ny) {
    float rx = px - r->x, ry = py - r->y;
    float vcx = r->vx - r->omega * ry, vcy = r->vy + r->omega * rx;
    float vn = vcx * nx + vcy * ny;
    if (vn >= 0.0f) {
        return; // moving apart already
    }
    float m = (float)r->pixels;
    float rn = rx * ny - ry * nx;
    float e = vn < -BOUNCE_MIN ? RESTITUTION : 0.0f;
    float jn = -(1.0f + e) * vn / (1.0f / m + rn * rn / r->inertia);
    impulse(r, px, py, jn * nx, jn * ny);
    // Friction along the surface.
    vcx = r->vx - r->omega * ry;
    vcy = r->vy + r->omega * rx;
    float tx = -ny, ty = nx, vt = vcx * tx + vcy * ty;
    float rt = rx * ty - ry * tx;
    float jt = -vt / (1.0f / m + rt * rt / r->inertia);
    jt = PR_CLAMP(jt, -FRICTION * jn, FRICTION * jn);
    impulse(r, px, py, jt * tx, jt * ty);
}

// One tick of motion in sub-steps of at most about a cell. Returns true if it touched anything.
static bool dynamics(World *w, Rigid *r) {
    r->vx = PR_CLAMP(r->vx, -MAX_SPEED, MAX_SPEED);
    r->vy = PR_CLAMP(r->vy, -MAX_SPEED, MAX_SPEED);
    float reach = sqrtf(r->vx * r->vx + r->vy * r->vy) + fabsf(r->omega) * r->radius;
    int n = PR_CLAMP((int)ceilf(reach), 1, MAX_SUBSTEPS);
    float dt = 1.0f / (float)n;
    bool touched = false;
    for (int s = 0; s < n; s++) {
        r->vy += PARTICLE_GRAVITY * dt;
        float x = r->x + r->vx * dt, y = r->y + r->vy * dt, a = r->angle + r->omega * dt;
        float nx = 0.0f, ny = -1.0f, cx = r->x, cy = r->y;
        int hits = collide(w, r, x, y, a, &nx, &ny, &cx, &cy);
        if (hits == 0) {
            r->x = x;
            r->y = y;
            r->angle = a;
            continue;
        }
        touched = true;
        // A touch of a few cells is let through (those pixels are not drawn while they overlap,
        // see update_one): the pose of a body resting on the knife-edge of a cell would never
        // advance otherwise. Anything deeper stops it at the surface.
        bool soft = hits > 0 && hits <= PR_MIN(SOFT_CELLS + r->pixels / 32, r->pixels / 2);
        if (soft) {
            r->x = x;
            r->y = y;
            r->angle = a;
        }
        // Go as far toward the blocked pose as it can without overlapping (halving the step).
        bool advanced = soft;
        for (float f = 0.5f; f > 0.07f && !soft; f *= 0.5f) {
            float fx = r->x + r->vx * dt * f, fy = r->y + r->vy * dt * f,
                  fa = r->angle + r->omega * dt * f, ox, oy, ocx, ocy;
            if (collide(w, r, fx, fy, fa, &ox, &oy, &ocx, &ocy) == 0) {
                r->x = fx;
                r->y = fy;
                r->angle = fa;
                advanced = true;
                break;
            }
        }
        if (hits < 0) {
            nx = -r->vx; // outside the world: stop it
            ny = -r->vy;
            float len = sqrtf(nx * nx + ny * ny);
            nx = len > 1e-6f ? nx / len : 0.0f;
            ny = len > 1e-6f ? ny / len : -1.0f;
        } else {
            contact_normal(w, r, x, y, a, &nx, &ny);
        }
        collision_impulse(r, cx, cy, nx, ny);
        if (!advanced && hits > 0) {
            // Stuck in an overlap too deep to take another step (no pose along the way is free):
            // ease it out along the contact normal, a quarter cell at a time.
            float ox, oy, ocx, ocy;
            int now = collide(w, r, r->x, r->y, r->angle, &ox, &oy, &ocx, &ocy);
            if (now != 0) {
                float px = r->x + nx * 0.25f, py = r->y + ny * 0.25f;
                int after = collide(w, r, px, py, r->angle, &ox, &oy, &ocx, &ocy);
                if (after >= 0 && after <= now) {
                    r->x = px;
                    r->y = py;
                }
            }
        }
    }
    if (touched) {
        r->omega *= SPIN_DAMPING;
        if (fabsf(r->vx) + fabsf(r->vy) + fabsf(r->omega) * r->radius < 0.2f) {
            r->vx *= RESTING_DAMPING; // nearly still and touching: friction takes the rest
            r->vy *= RESTING_DAMPING;
            r->omega *= RESTING_DAMPING;
        }
    }
    return touched;
}

static bool in_active_area(World *w, const Rigid *r) {
    int cx = PR_CLAMP((int)r->x, 0, w->width - 1) >> CHUNK_SHIFT;
    int cy = PR_CLAMP((int)r->y, 0, w->height - 1) >> CHUNK_SHIFT;
    return world_chunk(w, cx, cy)->active;
}

static void update_one(World *w, int ri) {
    Rigid *r = &w->rigids[ri];
    static RasterCell rc[RIGID_MAX_CELLS];
    if (r->asleep) {
        if ((w->tick + (uint64_t)ri) % WAKE_CHECK_TICKS != 0) {
            return;
        }
        if (readback(w, r)) {
            restructure(w, ri);
            return;
        }
        own_build(r);
        // Still held up? (Something one cell lower than the body is free: it would fall.)
        float nx, ny, cx, cy;
        g_support_check = true;
        int held = collide(w, r, r->x, r->y + 1.0f, r->angle, &nx, &ny, &cx, &cy);
        g_support_check = false;
        if (held != 0) {
            return;
        }
        r->asleep = 0;
        r->rest_ticks = 0;
    } else if (readback(w, r)) {
        restructure(w, ri);
        return;
    }
    own_build(r);
    float px0 = r->x, py0 = r->y, pa0 = r->angle;
    bool touched = dynamics(w, r);
    float fastest = sqrtf(r->vx * r->vx + r->vy * r->vy) + fabsf(r->omega) * r->radius;
    // A body that touches something and could not move at all is at rest too, whatever its
    // velocity: wedged, gravity and the contact can hold a velocity just above SLEEP_SPEED for
    // ever, and a body that never slept redrew itself and kept everything around it awake.
    bool stuck = r->x == px0 && r->y == py0 && r->angle == pa0;
    if (touched && (fastest < SLEEP_SPEED || stuck)) {
        if (++r->rest_ticks >= SLEEP_TICKS) {
            r->asleep = 1;
            r->vx = r->vy = r->omega = 0.0f;
        }
    } else {
        r->rest_ticks = 0;
    }
    int n = raster(w, r, r->x, r->y, r->angle, rc);
    if (n < 0) {
        return;
    }
    own_build(r);
    int kept = 0;
    for (int k = 0; k < n; k++) {
        if (free_for(w, rc[k].idx)) {
            rc[kept++] = rc[k]; // pixels over something solid are not drawn
        }
    }
    n = kept;
    if (n == 0) { // nothing of it can be drawn (it is all inside something solid): it is gone
        for (int k = 0; k < r->ncells; k++) {
            if (is_wood(w->cells[r->cell_idx[k]].element)) {
                clear_cell(w, r->cell_idx[k]);
                note(w, r->cell_idx[k]);
            }
        }
        r->active = 0;
        if (ri == w->rigid_count - 1) {
            w->rigid_count--;
        }
        return;
    }
    // Only if the grid shows something different.
    bool same = n == r->ncells;
    for (int k = 0; k < n && same; k++) {
        same = rc[k].idx == r->cell_idx[k] && rc[k].pix == r->cell_pix[k];
    }
    if (!same) {
        commit(w, r, rc, n);
    }
}

// A corpse that is still bleeding loses a droplet now and then, from a cell of its surface.
#define RIGID_BLEED_TICKS 5

static void bleed_one(World *w, Rigid *r, int ri) {
    uint64_t h = pr_splitmix64(w->seed ^ (w->tick * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)ri << 24));
    static const int8_t d4[4][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
    for (int attempt = 0; attempt < 12; attempt++) { // a cell of the surface: one with air beside it
        h = pr_splitmix64(h);
        int k = (int)((h >> 16) % (uint64_t)r->ncells);
        int x = (int)(r->cell_idx[k] % w->width), y = (int)(r->cell_idx[k] / w->width);
        int first = (int)(h >> 40) & 3;
        for (int q = 0; q < 4; q++) {
            int d = (first + q) & 3, nx = x + d4[d][0], ny = y + d4[d][1];
            if (!particle_passes(w, nx, ny)) {
                continue;
            }
            float jitter = (float)((h >> (8 + 4 * q)) & 15) / 15.0f;
            Cell cell = {ELEM_BLOOD, 0, (uint8_t)(h >> 8), 0};
            Particle p = {(float)nx + 0.5f, (float)ny + 0.5f,
                          (float)d4[d][0] * (0.15f + 0.35f * jitter),
                          (float)d4[d][1] * 0.2f - 0.1f * jitter, cell, T_ROOM};
            if (particle_add(w, &p)) {
                r->bleed -= 1.0f;
            }
            return;
        }
    }
}

void rigids_update(World *w) {
    for (int i = 0; i < w->rigid_count; i++) {
        Rigid *r = &w->rigids[i];
        if (!r->active || !in_active_area(w, r)) {
            continue;
        }
        update_one(w, i);
        r = &w->rigids[i];
        if (r->active && r->bleed > 0.0f && r->ncells > 0 &&
            (w->tick + (uint64_t)i) % RIGID_BLEED_TICKS == 0) {
            bleed_one(w, r, i);
        }
    }
}

int rigid_spawn_from_cells(World *w, const int32_t *cells, int n, ElementId material, float vx,
                           float vy, float bleed) {
    if (n <= 0 || n > RIGID_MAX_CELLS) {
        return -1;
    }
    int x0 = w->width, y0 = w->height, x1 = -1, y1 = -1;
    for (int k = 0; k < n; k++) {
        int x = (int)(cells[k] % w->width), y = (int)(cells[k] / w->width);
        x0 = PR_MIN(x0, x);
        x1 = PR_MAX(x1, x);
        y0 = PR_MIN(y0, y);
        y1 = PR_MAX(y1, y);
    }
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    if (bw > RIGID_MAX_DIM || bh > RIGID_MAX_DIM) {
        return -1;
    }
    int slot = new_slot(w);
    if (slot < 0) {
        return -1;
    }
    Rigid *r = &w->rigids[slot];
    memset(r, 0, sizeof(*r));
    r->w = (int16_t)bw;
    r->h = (int16_t)bh;
    static RasterCell rc[RIGID_MAX_CELLS];
    for (int k = 0; k < n; k++) {
        int x = (int)(cells[k] % w->width), y = (int)(cells[k] / w->width);
        int pi = (y - y0) * bw + (x - x0);
        r->pix[pi].cell = (Cell){material, 0, w->cells[cells[k]].shade, 0};
        r->pix[pi].temp = w->temp[cells[k]];
        rc[k].idx = cells[k];
        rc[k].pix = (uint16_t)pi;
    }
    props(r);
    r->x = (float)x0 + r->cx; // unturned, the bitmap's cells are the grid's
    r->y = (float)y0 + r->cy;
    r->vx = vx;
    r->vy = vy;
    r->bleed = bleed;
    r->active = 1;
    r->ncells = 0;
    commit(w, r, rc, n);
    restructure(w, slot); // in several pieces? (a monster cut in half)
    return w->rigids[slot].active ? slot : -1;
}

void rigids_free(World *w) {
    pr_aligned_free(w->rigids);
    w->rigids = NULL;
}
