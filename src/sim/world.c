#include "sim/world.h"

#include "core/log.h"
#include "sim/heat.h"
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
    w->mass = pr_aligned_alloc_zero(64, n * sizeof(float));
    w->piece = pr_aligned_alloc_zero(64, n * sizeof(uint8_t));
    w->chunks = pr_aligned_alloc_zero(64, (size_t)w->chunks_x * w->chunks_y * sizeof(Chunk));
    w->pressure = w->chunks ? pressure_create(w) : NULL;
    if (!w->cells || !w->temp || !w->temp_next || !w->mass || !w->piece || !w->chunks ||
        !w->pressure) {
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
    pr_aligned_free(w->mass);
    heat_free(w);
    pr_aligned_free(w->piece);
    pressure_destroy(w->pressure);
    pr_aligned_free(w->chunks);
    pr_aligned_free(w->sim_scratch);
    free(w);
}

void world_clear(World *w) {
    size_t n = (size_t)w->width * (size_t)w->height;
    for (size_t i = 0; i < n; i++) {
        w->cells[i] = (Cell){0};
        w->temp[i] = 0.0f;
        w->mass[i] = 0.0f;
        w->piece[i] = 0;
    }
    for (int i = 0; i < w->chunks_x * w->chunks_y; i++) {
        dirty_rect_clear(&w->chunks[i].cur);
        dirty_rect_clear(&w->chunks[i].next);
        w->chunks[i].temp_lo = 1e30f;
        w->chunks[i].temp_hi = -1e30f;
    }
    world_wake_rect(w, 0, 0, w->width - 1, w->height - 1);
    world_wake_heat(w, 0, 0, w->width - 1, w->height - 1);
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

void world_set_cell(World *w, int x, int y, ElementId e, uint32_t random_bits) {
    PR_ASSERT(world_in_bounds(w, x, y));
    PR_ASSERT(e < ELEM_COUNT);
    size_t i = world_index(w, x, y);
    Cell c;
    c.element = e;
    // Not equal to the stamp of the next tick, so the cell is simulated in it.
    c.stamp = (uint8_t)(w->tick - 1);
    c.shade = (uint8_t)random_bits;
    c.aux = (uint8_t)((random_bits >> 8) & 0x80); // random initial flow direction, zero speed
    w->cells[i] = c;
    w->temp[i] = g_elem_defs[e].spawn_temp;
    w->mass[i] = g_elem_behavior[e] == BEHAVIOR_GAS ? gas_mass_at(e, P_ATM, w->temp[i]) : 0.0f;
}

void world_set_gas(World *w, int x, int y, ElementId e, float m, uint32_t random_bits) {
    PR_ASSERT(g_elem_behavior[e] == BEHAVIOR_GAS && m > 0.0f);
    world_set_cell(w, x, y, e, random_bits);
    w->mass[world_index(w, x, y)] = m;
}

float world_gas_pressure(const World *w, int x, int y) {
    if (!world_in_bounds(w, x, y)) {
        return 0.0f;
    }
    size_t i = world_index(w, x, y);
    ElementId e = w->cells[i].element;
    if (g_elem_behavior[e] != BEHAVIOR_GAS) {
        return 0.0f;
    }
    return gas_pressure(e, w->mass[i], elem_temp(e, w->temp[i]));
}

#define REGION_SIDE (2 * GAS_REGION_RADIUS + 1)

int world_gas_region(const World *w, int x, int y, ElementId g, int ex, int ey, int radius, int max,
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
            ElementId e = world_elem(w, cx, cy);
            if (g_elem_behavior[e] != BEHAVIOR_GAS || (cx == ex && cy == ey)) {
                continue;
            }
            queue[tail][0] = (int8_t)dx;
            queue[tail][1] = (int8_t)dy;
            tail++;
            if (e == g) {
                found[n++] = (int32_t)world_index(w, cx, cy);
            }
        }
    }
    return n;
}

void world_gas_spread_add(World *w, const int32_t *found, int n, float m, float t) {
    float share = m / (float)n;
    for (int k = 0; k < n; k++) {
        size_t i = (size_t)found[k];
        float mi = w->mass[i];
        w->temp[i] = (mi * w->temp[i] + share * t) / (mi + share);
        w->mass[i] = mi + share;
    }
}

float world_gas_spread_take(World *w, const int32_t *found, int n, float *t) {
    float m = 0.0f, e = 0.0f;
    for (int k = 0; k < n; k++) {
        size_t i = (size_t)found[k];
        float dm = w->mass[i] / (float)(n + 1);
        w->mass[i] -= dm;
        m += dm;
        e += dm * w->temp[i];
    }
    *t = m > 0.0f ? e / m : 0.0f;
    return m;
}
