#include "sim/world.h"

#include "core/log.h"
#include "sim/creature.h"
#include "sim/heat.h"
#include "sim/particle.h"
#include "sim/pressure.h"

#include <stdlib.h>
#include <string.h>

World *world_create(int32_t width, int32_t height, uint64_t seed) {
    elements_init();
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    width = (width + CHUNK_MASK) & ~CHUNK_MASK;
    height = (height + CHUNK_MASK) & ~CHUNK_MASK;

    World *w = calloc(1, sizeof(World));
    if (!w) {
        return NULL;
    }
    w->width = width;
    w->height = height;
    w->chunks_x = width / CHUNK_SIZE;
    w->chunks_y = height / CHUNK_SIZE;
    w->seed = seed;
    size_t n = (size_t)width * (size_t)height;
    w->cells = pr_aligned_alloc_zero(64, n * sizeof(Cell));
    w->temp = pr_aligned_alloc_zero(64, n * sizeof(float));
    w->temp_next = pr_aligned_alloc_zero(64, n * sizeof(float));
    w->gas = pr_aligned_alloc_zero(64, n * sizeof(GasMix));
    w->rho = pr_aligned_alloc_zero(64, n * sizeof(float));
    w->piece = pr_aligned_alloc_zero(64, n * sizeof(uint8_t));
    w->chunks = pr_aligned_alloc_zero(64, (size_t)w->chunks_x * w->chunks_y * sizeof(Chunk));
    w->particles = pr_aligned_alloc_zero(64, PARTICLE_MAX * sizeof(Particle));
    w->creatures = pr_aligned_alloc_zero(64, CREATURE_MAX * sizeof(Creature));
    w->pressure = w->chunks ? pressure_create(w) : NULL;
    if (!w->cells || !w->temp || !w->temp_next || !w->gas || !w->rho || !w->particles ||
        !w->creatures || !w->piece || !w->chunks || !w->pressure) {
        pr_log_error("world_create: out of memory for %dx%d world", width, height);
        world_destroy(w);
        return NULL;
    }
    world_clear(w);
    return w;
}

void world_destroy(World *w) {
    if (!w) {
        return;
    }
    pr_aligned_free(w->cells);
    pr_aligned_free(w->temp);
    pr_aligned_free(w->temp_next);
    pr_aligned_free(w->gas);
    pr_aligned_free(w->rho);
    heat_free(w);
    pr_aligned_free(w->piece);
    pressure_destroy(w->pressure);
    pr_aligned_free(w->chunks);
    pr_aligned_free(w->particles);
    pr_aligned_free(w->creatures);
    pr_aligned_free(w->sim_scratch);
    free(w);
}

void world_clear(World *w) {
    size_t n = (size_t)w->width * (size_t)w->height;
    for (size_t i = 0; i < n; i++) {
        w->cells[i] = (Cell){0};
        w->temp[i] = 0.0f;
        gas_mix_clear(&w->gas[i]);
        w->rho[i] = 0.0f;
        w->piece[i] = 0;
    }
    w->particle_count = 0;
    w->creature_count = 0;
    for (int i = 0; i < w->chunks_x * w->chunks_y; i++) {
        dirty_rect_clear(&w->chunks[i].cur);
        dirty_rect_clear(&w->chunks[i].next);
        w->chunks[i].temp_lo = 1e30f;
        w->chunks[i].temp_hi = -1e30f;
        w->chunks[i].active = 1;
    }
    world_wake_rect(w, 0, 0, w->width - 1, w->height - 1);
    world_wake_heat(w, 0, 0, w->width - 1, w->height - 1);
}

void world_simulate_near(World *w, int x, int y) {
    const int per = SIM_REGION / CHUNK_SIZE; // chunks per region side
    int rx = PR_CLAMP(x, 0, w->width - 1) / SIM_REGION,
        ry = PR_CLAMP(y, 0, w->height - 1) / SIM_REGION;
    int cx0 = (rx - 1) * per, cx1 = (rx + 2) * per; // chunks [cx0, cx1)
    int cy0 = (ry - 1) * per, cy1 = (ry + 2) * per;
    for (int cy = 0; cy < w->chunks_y; cy++) {
        for (int cx = 0; cx < w->chunks_x; cx++) {
            world_chunk(w, cx, cy)->active = cx >= cx0 && cx < cx1 && cy >= cy0 && cy < cy1;
        }
    }
}

void world_simulate_all(World *w) {
    for (int i = 0; i < w->chunks_x * w->chunks_y; i++) {
        w->chunks[i].active = 1;
    }
}

void world_wake_heat(World *w, int x0, int y0, int x1, int y1) {
    x0 = PR_MAX(x0, 0);
    y0 = PR_MAX(y0, 0);
    x1 = PR_MIN(x1, w->width - 1);
    y1 = PR_MIN(y1, w->height - 1);
    for (int cy = y0 >> CHUNK_SHIFT; cy <= y1 >> CHUNK_SHIFT && x0 <= x1; cy++) {
        for (int cx = x0 >> CHUNK_SHIFT; cx <= x1 >> CHUNK_SHIFT; cx++) {
            world_chunk(w, cx, cy)->heat_active_next = 1;
        }
    }
}

void world_note_moved(World *w, int x, int y) {
    Cell c = w->cells[world_index(w, x, y)];
    if (g_elem_cap[c.element] == 0.0f) {
        return;
    }
    Chunk *ch = world_chunk(w, x >> CHUNK_SHIFT, y >> CHUNK_SHIFT);
    float t = elem_temp(c.element, w->temp[world_index(w, x, y)]);
    ch->temp_lo = PR_MIN(ch->temp_lo, t);
    ch->temp_hi = PR_MAX(ch->temp_hi, t);
    ch->heat_check = 1;
}

void world_wake_rect(World *w, int x0, int y0, int x1, int y1) {
    x0 = PR_MAX(x0, 0);
    y0 = PR_MAX(y0, 0);
    x1 = PR_MIN(x1, w->width - 1);
    y1 = PR_MIN(y1, w->height - 1);
    if (x0 > x1 || y0 > y1) {
        return;
    }
    int cx0 = x0 >> CHUNK_SHIFT, cx1 = x1 >> CHUNK_SHIFT;
    int cy0 = y0 >> CHUNK_SHIFT, cy1 = y1 >> CHUNK_SHIFT;
    for (int cy = cy0; cy <= cy1; cy++) {
        int base_y = cy << CHUNK_SHIFT;
        int ly0 = PR_MAX(y0 - base_y, 0);
        int ly1 = PR_MIN(y1 - base_y, CHUNK_SIZE - 1);
        for (int cx = cx0; cx <= cx1; cx++) {
            int base_x = cx << CHUNK_SHIFT;
            int lx0 = PR_MAX(x0 - base_x, 0);
            int lx1 = PR_MIN(x1 - base_x, CHUNK_SIZE - 1);
            Chunk *c = world_chunk(w, cx, cy);
            dirty_rect_add(&c->next, lx0, ly0, lx1, ly1);
            c->render_dirty = 1;
            c->head_dirty_next = 1;
        }
    }
}

GasMix gas_mix_air(float p, float t) {
    float n = p * CELL_VOLUME_M3 / (GAS_R * t);
    GasMix g;
    gas_mix_clear(&g);
    g.m[GAS_O2] = n * AIR_O2_FRAC * g_species_molar[GAS_O2];
    g.m[GAS_CO2] = n * AIR_CO2_FRAC * g_species_molar[GAS_CO2];
    g.m[GAS_N2] = n * (1.0f - AIR_O2_FRAC - AIR_CO2_FRAC) * g_species_molar[GAS_N2];
    return g;
}

void world_set_cell(World *w, int x, int y, ElementId e, uint32_t random_bits) {
    PR_ASSERT(world_in_bounds(w, x, y));
    PR_ASSERT(e < ELEM_COUNT);
    size_t i = world_index(w, x, y);
    int s = elem_species(e);
    bool gas = g_elem_behavior[e] == BEHAVIOR_GAS;
    Cell c;
    c.element = gas ? ELEM_GAS : e;
    // Not equal to the stamp of the next tick, so the cell is simulated in it.
    c.stamp = (uint8_t)(w->tick - 1);
    c.shade = (uint8_t)random_bits;
    // Liquids: random initial flow direction, zero speed. Gases: no current.
    c.aux = gas ? 0 : (uint8_t)((random_bits >> 8) & 0x80);
    w->cells[i] = c;
    w->temp[i] = g_elem_defs[e].spawn_temp;
    if (s >= 0) {
        w->gas[i] = gas_mix_pure(s, gas_mass_at(e, P_ATM, w->temp[i]));
    } else if (gas) {
        w->gas[i] = gas_mix_air(P_ATM, w->temp[i]);
    } else {
        gas_mix_clear(&w->gas[i]);
    }
}

void world_set_gas(World *w, int x, int y, ElementId e, float m, uint32_t random_bits) {
    PR_ASSERT(elem_species(e) >= 0 && m > 0.0f);
    world_set_cell(w, x, y, e, random_bits);
    w->gas[world_index(w, x, y)] = gas_mix_pure(elem_species(e), m);
}

void world_set_mix(World *w, int x, int y, const GasMix *g, float t, uint32_t random_bits) {
    world_set_cell(w, x, y, ELEM_GAS, random_bits);
    size_t i = world_index(w, x, y);
    w->gas[i] = *g;
    w->temp[i] = t;
}

float world_gas_pressure(const World *w, int x, int y) {
    if (!world_in_bounds(w, x, y)) {
        return 0.0f;
    }
    size_t i = world_index(w, x, y);
    if (w->cells[i].element != ELEM_GAS) {
        return 0.0f;
    }
    return gas_mix_pressure(&w->gas[i], w->temp[i]);
}

float world_gas_partial_pressure(const World *w, int x, int y, int s) {
    if (!world_in_bounds(w, x, y)) {
        return 0.0f;
    }
    size_t i = world_index(w, x, y);
    if (w->cells[i].element != ELEM_GAS) {
        return 0.0f;
    }
    return gas_pressure_moles(w->gas[i].m[s] * g_species_inv_molar[s], w->temp[i]);
}

#define REGION_SIDE (2 * GAS_REGION_RADIUS + 1)

int world_gas_region(const World *w, int x, int y, int ex, int ey, int radius, int max,
                     int32_t *found) {
    PR_ASSERT(radius <= GAS_REGION_RADIUS && max <= GAS_REGION_CELLS);
    uint8_t seen[REGION_SIDE][REGION_SIDE];
    int8_t queue[REGION_SIDE * REGION_SIDE][2];
    memset(seen, 0, sizeof(seen));
    int head = 0, tail = 0, n = 0;
    queue[tail][0] = 0;
    queue[tail][1] = 0;
    tail++;
    seen[GAS_REGION_RADIUS][GAS_REGION_RADIUS] = 1;
    static const int8_t d4[4][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
    while (head < tail && n < max) {
        int ox = queue[head][0], oy = queue[head][1];
        head++;
        for (int k = 0; k < 4 && n < max; k++) {
            int dx = ox + d4[k][0], dy = oy + d4[k][1];
            if (dx < -radius || dx > radius || dy < -radius || dy > radius ||
                seen[dy + GAS_REGION_RADIUS][dx + GAS_REGION_RADIUS]) {
                continue;
            }
            seen[dy + GAS_REGION_RADIUS][dx + GAS_REGION_RADIUS] = 1;
            int cx = x + dx, cy = y + dy;
            if (world_elem(w, cx, cy) != ELEM_GAS || (cx == ex && cy == ey)) {
                continue;
            }
            queue[tail][0] = (int8_t)dx;
            queue[tail][1] = (int8_t)dy;
            tail++;
            found[n++] = (int32_t)world_index(w, cx, cy);
        }
    }
    return n;
}

void world_gas_spread_add(World *w, const int32_t *found, int n, const GasMix *g, float t) {
    float f = 1.0f / (float)n;
    float ca = gas_heat_cap(g) * f;
    for (int k = 0; k < n; k++) {
        size_t i = (size_t)found[k];
        w->temp[i] = gas_mix_temp(gas_heat_cap(&w->gas[i]), w->temp[i], ca, t);
        gas_mix_add(&w->gas[i], g, f);
    }
}

void world_gas_spread_take(World *w, const int32_t *found, int n, GasMix *out, float *t) {
    gas_mix_clear(out);
    float c = 0.0f, e = 0.0f, f = 1.0f / (float)(n + 1);
    for (int k = 0; k < n; k++) {
        size_t i = (size_t)found[k];
        GasMix part = w->gas[i];
        gas_mix_scale(&part, f);
        gas_mix_add(&w->gas[i], &part, -1.0f);
        gas_mix_add(out, &part, 1.0f);
        float cp = gas_heat_cap(&part);
        c += cp;
        e += cp * w->temp[i];
    }
    *t = c > 0.0f ? e / c : T_ROOM;
}

bool world_gas_emit(World *w, int x, int y, const GasMix *g, float t, uint32_t random_bits) {
    if (!world_in_bounds(w, x, y) || gas_total(g) <= 0.0f) {
        return false;
    }
    size_t i = world_index(w, x, y);
    ElementId here = w->cells[i].element;
    if (here == ELEM_GAS) {
        int32_t self = (int32_t)i;
        world_gas_spread_add(w, &self, 1, g, t);
    } else if (here == ELEM_VOID) {
        world_set_mix(w, x, y, g, t, random_bits);
    } else {
        return false;
    }
    world_note_moved(w, x, y);
    world_wake_rect(w, x - 1, y - 1, x + 1, y + 1);
    world_wake_heat(w, x - 1, y - 1, x + 1, y + 1);
    return true;
}
