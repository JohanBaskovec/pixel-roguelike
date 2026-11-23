#include "sim/heat.h"

#include "core/jobs.h"

#include <math.h>
#include <stdlib.h>

// A chunk stays heat-active while some temperature in it changes by more than this per tick.
#define HEAT_SLEEP_K 1e-3f
// Temperature changes that may start convection: simulate the chunk's fluids this tick.
#define HEAT_WAKE_MOVEMENT_K 0.02f
// Hottest cell above which a chunk glows (and must be redrawn when temperatures change).
#define GLOW_START_K 750.0f
// When liquid boils, the new gas cell is far lighter (no gas mass until M4), so leftover heat
// would make it absurdly hot: cap how far above the boiling point it starts.
#define BOIL_EXCESS_CAP_K 20.0f

typedef struct HeatJob {
    World *w;
    int chunk;
    float max_change;  // largest |temperature change| this tick
    float max_temp;    // hottest physical temperature after the tick
    uint8_t fluid;     // the chunk holds liquid or gas
    uint16_t phase;    // cells that changed phase
    DirtyRect changed; // cells that changed phase (chunk-local)
    float lo, hi;      // range of physical temperatures after the tick
} HeatJob;

typedef struct HeatScratch {
    HeatJob *jobs;   // one per chunk
    uint8_t *marked; // per chunk: in this tick's set
} HeatScratch;

#define HALO (CHUNK_SIZE + 2)

static void compute_chunk(void *user, int index) {
    HeatJob *job = &((HeatJob *)user)[index];
    World *w = job->w;
    const uint8_t *marked = ((HeatScratch *)w->heat_scratch)->marked;
    const int bx = (job->chunk % w->chunks_x) << CHUNK_SHIFT;
    const int by = (job->chunk / w->chunks_x) << CHUNK_SHIFT;

    // Local copy of the chunk plus a one-cell border: element and physical temperature. Border
    // cells outside the world, or in a chunk not computed this tick, count as void (no exchange:
    // only pairs computed on both sides exchange heat, so energy cannot leak).
    ElementId el[HALO][HALO];
    float tp[HALO][HALO];
    for (int ly = -1; ly <= CHUNK_SIZE; ly++) {
        for (int lx = -1; lx <= CHUNK_SIZE; lx++) {
            int x = bx + lx, y = by + ly;
            ElementId e = ELEM_VOID;
            float t = 0.0f;
            bool inside = lx >= 0 && ly >= 0 && lx < CHUNK_SIZE && ly < CHUNK_SIZE;
            if (world_in_bounds(w, x, y) &&
                (inside || marked[(y >> CHUNK_SHIFT) * w->chunks_x + (x >> CHUNK_SHIFT)])) {
                size_t i = world_index(w, x, y);
                e = w->cells[i].element;
                t = elem_temp(e, w->temp[i]);
            }
            el[ly + 1][lx + 1] = e;
            tp[ly + 1][lx + 1] = t;
        }
    }

    float max_change = 0.0f, max_temp = 0.0f;
    uint8_t fluid = 0;
    for (int ly = 0; ly < CHUNK_SIZE; ly++) {
        size_t row = world_index(w, bx, by + ly);
        for (int lx = 0; lx < CHUNK_SIZE; lx++) {
            int hy = ly + 1, hx = lx + 1;
            ElementId e = el[hy][hx];
            size_t i = row + (size_t)lx;
            if (g_elem_cap[e] == 0.0f) {
                w->temp_next[i] = w->temp[i];
                continue;
            }
            fluid |= (uint8_t)elem_is_fluid(e);
            // Flux from each neighbour, using physical temperatures. Each pair computes the same
            // coefficient and opposite differences, so whatever one cell gains its neighbour
            // loses: energy is conserved exactly.
            const float *coef = g_heat_coef[e];
            float ti = tp[hy][hx];
            float sum = coef[el[hy][hx - 1]] * (tp[hy][hx - 1] - ti) +
                        coef[el[hy][hx + 1]] * (tp[hy][hx + 1] - ti) +
                        coef[el[hy - 1][hx]] * (tp[hy - 1][hx] - ti) +
                        coef[el[hy + 1][hx]] * (tp[hy + 1][hx] - ti);
            float change = sum * g_elem_inv_cap[e];
            w->temp_next[i] = w->temp[i] + change;
            max_change = PR_MAX(max_change, fabsf(change));
            max_temp = PR_MAX(max_temp, ti + change);
        }
    }
    job->max_change = max_change;
    job->max_temp = max_temp;
    job->fluid = fluid;
}

// Melting, freezing, boiling, condensing. The stored temperature holds latent heat: a cell
// changes phase once the heat beyond the transition point pays for it, and keeps what is left.
static void apply_chunk(void *user, int index) {
    HeatJob *job = &((HeatJob *)user)[index];
    World *w = job->w;
    const int bx = (job->chunk % w->chunks_x) << CHUNK_SHIFT;
    const int by = (job->chunk / w->chunks_x) << CHUNK_SHIFT;
    job->phase = 0;
    dirty_rect_clear(&job->changed);
    float lo = 1e30f, hi = -1e30f;
    for (int y = by; y < by + CHUNK_SIZE; y++) {
        for (int x = bx; x < bx + CHUNK_SIZE; x++) {
            size_t i = world_index(w, x, y);
            float t = w->temp_next[i];
            w->temp[i] = t;
            Cell c = w->cells[i];
            ElementId e = c.element;
            const PhaseChange *up = &g_elem_above[e < ELEM_COUNT ? e : 0];
            const PhaseChange *down = &g_elem_below[e < ELEM_COUNT ? e : 0];
            float cp = g_elem_defs[e < ELEM_COUNT ? e : 0].heat_cap;
            ElementId to = e;
            float t_new = t;
            if (up->temp > 0.0f && t > up->temp && cp * (t - up->temp) >= up->latent) {
                to = up->to;
                float rest = g_elem_mass[e] * (cp * (t - up->temp) - up->latent); // J
                t_new = up->temp + rest * g_elem_inv_cap[to];
                if (g_elem_behavior[to] == BEHAVIOR_GAS && g_elem_behavior[e] != BEHAVIOR_GAS) {
                    t_new = PR_MIN(t_new, up->temp + BOIL_EXCESS_CAP_K);
                }
            } else if (down->temp > 0.0f && t < down->temp &&
                       cp * (down->temp - t) >= down->latent) {
                to = down->to;
                float rest = g_elem_mass[e] * (cp * (down->temp - t) - down->latent);
                t_new = down->temp - rest * g_elem_inv_cap[to];
            }
            if (to != e) {
                c.element = to;
                c.aux &= 0x80; // at rest (a new liquid or gas cell starts still)
                c.stamp = (uint8_t)(w->tick - 1);
                w->cells[i] = c;
                w->temp[i] = t_new;
                job->phase++;
                int lx = x - bx, ly = y - by;
                dirty_rect_add(&job->changed, lx, ly, lx, ly);
            }
            if (g_elem_cap[c.element] > 0.0f) {
                float tp = elem_temp(c.element, w->temp[i]);
                lo = PR_MIN(lo, tp);
                hi = PR_MAX(hi, tp);
            }
        }
    }
    job->lo = lo;
    job->hi = hi;
}

// Wake cells for this tick's movement pass (heat runs before it), with their 3x3 neighbourhood.
static void wake_now(World *w, int x0, int y0, int x1, int y1) {
    x0 = PR_MAX(x0, 0);
    y0 = PR_MAX(y0, 0);
    x1 = PR_MIN(x1, w->width - 1);
    y1 = PR_MIN(y1, w->height - 1);
    for (int cy = y0 >> CHUNK_SHIFT; cy <= y1 >> CHUNK_SHIFT; cy++) {
        for (int cx = x0 >> CHUNK_SHIFT; cx <= x1 >> CHUNK_SHIFT; cx++) {
            Chunk *c = world_chunk(w, cx, cy);
            int bx = cx << CHUNK_SHIFT, by = cy << CHUNK_SHIFT;
            dirty_rect_add(&c->cur, PR_MAX(x0 - bx, 0), PR_MAX(y0 - by, 0),
                           PR_MIN(x1 - bx, CHUNK_SIZE - 1), PR_MIN(y1 - by, CHUNK_SIZE - 1));
            c->render_dirty = 1;
            c->head_dirty = 1; // water pressure relabels this tick (it runs after heat)
            c->heat_active_next = 1;
        }
    }
}

void heat_free(World *w) {
    HeatScratch *hs = w->heat_scratch;
    if (hs) {
        pr_aligned_free(hs->jobs);
        pr_aligned_free(hs->marked);
        free(hs);
        w->heat_scratch = NULL;
    }
}

void heat_update(World *w, HeatStats *stats) {
    int n_chunks = w->chunks_x * w->chunks_y;
    HeatScratch *hs = w->heat_scratch;
    if (!hs) {
        hs = calloc(1, sizeof(HeatScratch));
        PR_ASSERT_ALWAYS(hs != NULL);
        hs->jobs = pr_aligned_alloc_zero(64, sizeof(HeatJob) * (size_t)n_chunks);
        hs->marked = pr_aligned_alloc_zero(64, (size_t)n_chunks);
        PR_ASSERT_ALWAYS(hs->jobs && hs->marked);
        w->heat_scratch = hs;
    }

    // This tick's set: heat-active chunks and their neighbours (a neighbour's border cells
    // exchange heat with the active chunk, so both sides must be computed to conserve energy).
    for (int i = 0; i < n_chunks; i++) {
        hs->marked[i] = 0;
    }
    for (int cy = 0; cy < w->chunks_y; cy++) {
        for (int cx = 0; cx < w->chunks_x; cx++) {
            if (!world_chunk(w, cx, cy)->heat_active) {
                continue;
            }
            hs->marked[cy * w->chunks_x + cx] = 1;
            if (cx > 0) {
                hs->marked[cy * w->chunks_x + cx - 1] = 1;
            }
            if (cx + 1 < w->chunks_x) {
                hs->marked[cy * w->chunks_x + cx + 1] = 1;
            }
            if (cy > 0) {
                hs->marked[(cy - 1) * w->chunks_x + cx] = 1;
            }
            if (cy + 1 < w->chunks_y) {
                hs->marked[(cy + 1) * w->chunks_x + cx] = 1;
            }
        }
    }
    int n = 0;
    for (int i = 0; i < n_chunks; i++) {
        if (hs->marked[i]) {
            hs->jobs[n++] = (HeatJob){.w = w, .chunk = i};
        }
    }
    if (stats) {
        stats->chunks = (uint32_t)n;
        stats->phase_changes = 0;
    }
    if (n == 0) {
        return;
    }

    // Jacobi: every new temperature is computed from the old ones (into temp_next), then all are
    // written back. Both passes are parallel; neither depends on the order of jobs.
    jobs_run(w->jobs, n, compute_chunk, hs->jobs);
    jobs_run(w->jobs, n, apply_chunk, hs->jobs);

    for (int j = 0; j < n; j++) {
        HeatJob *job = &hs->jobs[j];
        int cx = job->chunk % w->chunks_x, cy = job->chunk / w->chunks_x;
        int bx = cx << CHUNK_SHIFT, by = cy << CHUNK_SHIFT;
        Chunk *c = &w->chunks[job->chunk];
        c->temp_lo = job->lo;
        c->temp_hi = job->hi;
        if (job->max_change > HEAT_SLEEP_K) {
            c->heat_active_next = 1;
            if (cx > 0) {
                c[-1].heat_active_next = 1;
            }
            if (cx + 1 < w->chunks_x) {
                c[1].heat_active_next = 1;
            }
            if (cy > 0) {
                c[-w->chunks_x].heat_active_next = 1;
            }
            if (cy + 1 < w->chunks_y) {
                c[w->chunks_x].heat_active_next = 1;
            }
        }
        if (job->max_change > HEAT_WAKE_MOVEMENT_K && job->fluid) {
            dirty_rect_add(&c->cur, 0, 0, CHUNK_SIZE - 1, CHUNK_SIZE - 1); // convection
        }
        if (job->max_temp >= GLOW_START_K && job->max_change > 0.05f) {
            c->render_dirty = 1;
        }
        if (job->phase) {
            DirtyRect *r = &job->changed;
            wake_now(w, bx + r->min_x - 1, by + r->min_y - 1, bx + r->max_x + 1, by + r->max_y + 1);
            if (stats) {
                stats->phase_changes += job->phase;
            }
        }
    }
}
