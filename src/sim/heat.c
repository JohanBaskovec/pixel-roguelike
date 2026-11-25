#include "sim/heat.h"

#include "core/jobs.h"
#include "core/rng.h"

#include <math.h>
#include <stdlib.h>

// A chunk stays heat-active while some temperature in it changes by more than this per tick.
#define HEAT_SLEEP_K 1e-3f
// Temperature changes that may start convection: simulate the chunk's fluids this tick.
#define HEAT_WAKE_MOVEMENT_K 0.02f
// Hottest cell above which a chunk glows (and must be redrawn when temperatures change).
#define GLOW_START_K 750.0f

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

// Heat flowing into a cell (conductivity ka, thermal mass ca, temperature ta) from a neighbour,
// when either is a gas: a gas cell's conductivity and thermal mass follow its mixture, so the
// coefficient (two half-cells in series, sped up by HEAT_TIME_SCALE_GAS) and the stability limit
// are computed here, not taken from g_heat_coef. 0.125 * min(ca, cb) is at most the table's limit
// (0.25 * ca * cb / (ca + cb)) and keeps a cell's four exchanges from overshooting. Symmetric.
//
// Between two gas cells, a current (gas wind, in either cell) mixes heat by turbulence: the
// coefficient is multiplied by 1 + GAS_TURB_PER_WIND * speed (docs/SIMULATION.md -> Heat pass;
// D-034). Most currents then exchange at the stability limit.
#define GAS_COND_SCALE (CELL_SIZE_M * TICK_SECONDS * HEAT_TIME_SCALE_GAS)

static inline float gas_flux(float ka, float ca, float ta, uint8_t wa, bool ga, float kb, float cb,
                             float tb, uint8_t wb, bool gb, float boost) {
    if (ka <= 0.0f || kb <= 0.0f || ca <= 0.0f || cb <= 0.0f) {
        return 0.0f;
    }
    float k = 2.0f * ka * kb / (ka + kb) * GAS_COND_SCALE * boost;
    if ((wa | wb) != 0 && ga && gb) {
        k *= 1.0f + GAS_TURB_PER_WIND * (float)PR_MAX(wa, wb);
    }
    k = PR_MIN(k, 0.125f * PR_MIN(ca, cb));
    return k * (tb - ta);
}

// Conductivity of a gas mixture: the mole-fraction-weighted mean of its species' (an
// approximation).
static inline float gas_mix_conduct(const GasMix *g) {
    float n = 0.0f, k = 0.0f;
    for (int s = 0; s < GAS_SPECIES; s++) {
        float ns = g->m[s] * g_species_inv_molar[s];
        n += ns;
        k += ns * g_species_k[s];
    }
    return n > 0.0f ? k / n : 0.0f;
}

// Heat flowing into halo cell (ay, ax) from its neighbour (by, bx) this tick (J): the table's
// coefficient between two cells that are not gas, else gas_flux. Equal temperatures (still gas
// at one temperature, the common case) cost no division.
// Local copy of a chunk plus a one-cell border (compute_chunk).
typedef struct HeatHalo {
    ElementId el[HALO][HALO];
    float tp[HALO][HALO];     // physical temperature
    float cap[HALO][HALO];    // thermal mass, J/K (gases: from their mixture)
    float cond[HALO][HALO];   // conductivity, W/(m K) (gases: from their mixture)
    uint8_t wind[HALO][HALO]; // gases: the current's speed (0..GAS_WIND_MAX)
} HeatHalo;

static inline float pair_flux(const HeatHalo *h, int ay, int ax, int by, int bx) {
    float ta = h->tp[ay][ax], tb = h->tp[by][bx];
    if (ta == tb) {
        return 0.0f;
    }
    ElementId ea = h->el[ay][ax], eb = h->el[by][bx];
    bool ga = ea == ELEM_GAS, gb = eb == ELEM_GAS;
    if (!ga && !gb) {
        return g_heat_coef[ea][eb] * (tb - ta);
    }
    // Hot gas heats wood far faster than still air conducts (flames and hot gas convect and
    // radiate): WOOD_GAS_COUPLING times, up to the stability limit.
    bool wood = ea == ELEM_WOOD || ea == ELEM_BURNING_WOOD || eb == ELEM_WOOD ||
                eb == ELEM_BURNING_WOOD;
    return gas_flux(h->cond[ay][ax], h->cap[ay][ax], ta, h->wind[ay][ax], ga, h->cond[by][bx],
                    h->cap[by][bx], tb, h->wind[by][bx], gb, wood ? WOOD_GAS_COUPLING : 1.0f);
}

static void compute_chunk(void *user, int index) {
    HeatJob *job = &((HeatJob *)user)[index];
    World *w = job->w;
    const uint8_t *marked = ((HeatScratch *)w->heat_scratch)->marked;
    const int bx = (job->chunk % w->chunks_x) << CHUNK_SHIFT;
    const int by = (job->chunk / w->chunks_x) << CHUNK_SHIFT;

    // Local copy of the chunk plus a one-cell border: element and physical temperature. Border
    // cells outside the world, or in a chunk not computed this tick, count as void (no exchange:
    // only pairs computed on both sides exchange heat, so energy cannot leak).
    HeatHalo halo;
    ElementId(*el)[HALO] = halo.el;
    float(*tp)[HALO] = halo.tp;
    float(*cap)[HALO] = halo.cap;
    float(*cond)[HALO] = halo.cond;
    uint8_t(*wind)[HALO] = halo.wind;
    for (int ly = -1; ly <= CHUNK_SIZE; ly++) {
        for (int lx = -1; lx <= CHUNK_SIZE; lx++) {
            int x = bx + lx, y = by + ly;
            ElementId e = ELEM_VOID;
            float t = 0.0f, k = 0.0f, kc = 0.0f;
            uint8_t wd = 0;
            bool inside = lx >= 0 && ly >= 0 && lx < CHUNK_SIZE && ly < CHUNK_SIZE;
            if (world_in_bounds(w, x, y) &&
                (inside || marked[(y >> CHUNK_SHIFT) * w->chunks_x + (x >> CHUNK_SHIFT)])) {
                size_t i = world_index(w, x, y);
                Cell cell = w->cells[i];
                e = cell.element;
                t = elem_temp(e, w->temp[i]);
                bool gas = e == ELEM_GAS;
                k = gas ? gas_heat_cap(&w->gas[i]) : g_elem_cap[e];
                kc = gas ? gas_mix_conduct(&w->gas[i]) : g_elem_defs[e].conduct;
                wd = gas ? (uint8_t)gas_wind_speed(cell.aux) : 0;
            }
            el[ly + 1][lx + 1] = e;
            tp[ly + 1][lx + 1] = t;
            cap[ly + 1][lx + 1] = k;
            cond[ly + 1][lx + 1] = kc;
            wind[ly + 1][lx + 1] = wd;
        }
    }

    float max_change = 0.0f, max_temp = 0.0f;
    uint8_t fluid = 0;
    // Each pair of neighbours is computed once (heat into the first cell from the second):
    // horizontal pairs along the row, vertical pairs with the row below. What one cell gains its
    // neighbour loses: energy is conserved exactly.
    float down_prev[CHUNK_SIZE]; // heat into the row above from this row, per column
    for (int lx = 0; lx < CHUNK_SIZE; lx++) {
        down_prev[lx] = pair_flux(&halo, 0, lx + 1, 1, lx + 1);
    }
    for (int ly = 0; ly < CHUNK_SIZE; ly++) {
        size_t row = world_index(w, bx, by + ly);
        int hy = ly + 1;
        float right_prev = pair_flux(&halo, hy, 0, hy, 1); // into the left halo
        for (int lx = 0; lx < CHUNK_SIZE; lx++) {
            int hx = lx + 1;
            float from_right = pair_flux(&halo, hy, hx, hy, hx + 1);
            float from_below = pair_flux(&halo, hy, hx, hy + 1, hx);
            float sum = from_right - right_prev + from_below - down_prev[lx];
            right_prev = from_right;
            down_prev[lx] = from_below;
            size_t i = row + (size_t)lx;
            float ci = cap[hy][hx];
            if (ci == 0.0f) {
                w->temp_next[i] = w->temp[i];
                continue;
            }
            fluid |= (uint8_t)elem_is_fluid(el[hy][hx]);
            float change = sum / ci;
            w->temp_next[i] = w->temp[i] + change;
            max_change = PR_MAX(max_change, fabsf(change));
            max_temp = PR_MAX(max_temp, tp[hy][hx] + change);
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
            // Gas cells have no phase change of their own (water vapour condenses in sim.c).
            float m = g_elem_mass[e];
            if (up->temp > 0.0f && t > up->temp && cp * (t - up->temp) >= up->latent) {
                // Melting or boiling. Boiling water keeps its mass: one cell of water vapour at
                // very high pressure, which then expands (about 1700 cells at 1 atm).
                to = up->to;
                float rest = m * (cp * (t - up->temp) - up->latent); // J
                float cp_new = to == ELEM_GAS ? g_species_cp[GAS_H2O] : g_elem_cp[to];
                float m_new = to == ELEM_GAS ? m : g_elem_mass[to];
                t_new = up->temp + rest / (m_new * cp_new);
            } else if (down->temp > 0.0f && t < down->temp &&
                       cp * (down->temp - t) >= down->latent) {
                to = down->to;
                float rest = m * (cp * (down->temp - t) - down->latent);
                t_new = down->temp - rest / (g_elem_mass[to] * g_elem_cp[to]);
            }
            if (to != e) {
                c.element = to;
                // At rest: a new liquid keeps its flow direction, a new gas has no current.
                bool wood = (e == ELEM_WOOD || e == ELEM_BURNING_WOOD) &&
                            (to == ELEM_WOOD || to == ELEM_BURNING_WOOD);
                c.aux = wood ? c.aux // how much has burnt stays
                             : (g_elem_behavior[to] == BEHAVIOR_GAS ? 0 : (uint8_t)(c.aux & 0x80));
                c.stamp = (uint8_t)(w->tick - 1);
                w->cells[i] = c;
                w->temp[i] = t_new;
                if (to == ELEM_GAS) {
                    w->gas[i] = gas_mix_pure(GAS_H2O, m);
                }
                job->phase++;
                int lx = x - bx, ly = y - by;
                dirty_rect_add(&job->changed, lx, ly, lx, ly);
            }
            if (c.element != ELEM_VOID) {
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
            chunk_wake_now(c, PR_MAX(x0 - bx, 0), PR_MAX(y0 - by, 0),
                           PR_MIN(x1 - bx, CHUNK_SIZE - 1), PR_MIN(y1 - by, CHUNK_SIZE - 1));
            c->render_dirty = 1;
            if (c->active) {
                c->head_dirty = 1; // water pressure relabels this tick (it runs after heat)
            } else {
                c->head_dirty_next = 1; // ... or once it is simulated again
            }
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
    // exchange heat with the active chunk, so both sides must be computed to conserve energy),
    // only where the world is simulated (a frozen chunk counts as not computed: no exchange).
    for (int i = 0; i < n_chunks; i++) {
        hs->marked[i] = 0;
    }
    for (int cy = 0; cy < w->chunks_y; cy++) {
        for (int cx = 0; cx < w->chunks_x; cx++) {
            if (!world_chunk(w, cx, cy)->heat_active) {
                continue;
            }
            static const int8_t d5[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (int k = 0; k < 5; k++) {
                int nx = cx + d5[k][0], ny = cy + d5[k][1];
                if (nx >= 0 && ny >= 0 && nx < w->chunks_x && ny < w->chunks_y &&
                    world_chunk(w, nx, ny)->active) {
                    hs->marked[ny * w->chunks_x + nx] = 1;
                }
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
            c->head_dirty_next = 1; // gas pressure on liquid surfaces may have changed
        }
        // Glowing matter, and air tinted red or blue (render.c) once it is dangerous, are redrawn
        // as they change.
        if ((job->max_temp >= GLOW_START_K || job->hi >= T_HOT_DANGER ||
             job->lo <= T_COLD_DANGER_BEAST) &&
            job->max_change > 0.05f) {
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
