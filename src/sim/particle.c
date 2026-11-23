#include "sim/particle.h"

#include <math.h>

bool particle_add(World *w, const Particle *p) {
    if (w->particle_count >= PARTICLE_MAX) {
        return false;
    }
    w->particles[w->particle_count++] = *p;
    return true;
}

// Puts the particle's cell at (x, y), which must pass (void or gas). Gas there is squeezed into
// the gas of its kind around (spread out, as when water flows into gas). Returns false if that
// gas has nowhere to go.
static bool place(World *w, const Particle *p, int x, int y) {
    size_t i = world_index(w, x, y);
    ElementId here = w->cells[i].element;
    if (here != ELEM_VOID) {
        int32_t found[GAS_REGION_CELLS];
        int n = world_gas_region(w, x, y, here, x, y, GAS_REGION_RADIUS, GAS_REGION_CELLS, found);
        if (n == 0) {
            return false;
        }
        world_gas_spread_add(w, found, n, w->mass[i], w->temp[i]);
        for (int k = 0; k < n; k++) {
            world_note_moved(w, found[k] % w->width, found[k] / w->width);
        }
        world_wake_rect(w, x - GAS_REGION_RADIUS, y - GAS_REGION_RADIUS, x + GAS_REGION_RADIUS,
                        y + GAS_REGION_RADIUS);
    }
    Cell c = p->cell;
    c.stamp = (uint8_t)w->tick; // simulated in the next tick
    // Still falling if nothing is below it (it hit something sideways): keep its fall speed.
    int speed = 0;
    if (p->vy > 0.0f && particle_passes(w, x, y + 1)) {
        speed = (int)PR_CLAMP(p->vy * 16.0f, 1.0f, 127.0f);
    }
    c.aux = (uint8_t)((p->vx > 0.0f ? 0x80 : 0) | speed);
    w->cells[i] = c;
    w->temp[i] = p->temp;
    w->mass[i] = 0.0f;
    world_wake_rect(w, x - 1, y - 1, x + 1, y + 1);
    world_wake_heat(w, x, y, x, y);
    world_note_moved(w, x, y);
    return true;
}

// Lands at (x, y), or else the nearest cell that passes: straight up first (liquid piles up),
// then rings of growing size around it, up to LAND_SEARCH cells away (a crowded spot, e.g. a
// jet's hole filling up).
#define LAND_SEARCH 8

static bool land(World *w, Particle *p, int x, int y) {
    for (int k = 0; k <= LAND_SEARCH; k++) {
        if (!particle_passes(w, x, y - k)) {
            break;
        }
        if (place(w, p, x, y - k)) {
            return true;
        }
    }
    for (int r = 1; r <= LAND_SEARCH; r++) {
        for (int dy = -r; dy <= r; dy++) { // top row first
            for (int dx = -r; dx <= r; dx++) {
                if ((dx != -r && dx != r && dy != -r && dy != r) ||
                    !particle_passes(w, x + dx, y + dy)) {
                    continue;
                }
                if (place(w, p, x + dx, y + dy)) {
                    return true;
                }
            }
        }
    }
    // Last resort, in a crowded pocket inside liquid: up through the liquid to the first cell
    // that passes (the surface), as a water-pressure transfer would move it.
    for (int yy = y - LAND_SEARCH - 1; yy >= 0; yy--) {
        if (particle_passes(w, x, yy)) {
            if (place(w, p, x, yy)) {
                return true;
            }
            break;
        }
        if (g_elem_behavior[world_elem(w, x, yy)] != BEHAVIOR_LIQUID) {
            break;
        }
    }
    // Nowhere to land this tick: wait in place and try again.
    p->vx = p->vy = 0.0f;
    return false;
}

bool particle_land_now(World *w, Particle *p) {
    return land(w, p, (int)floorf(p->x), (int)floorf(p->y));
}

bool particle_step(World *w, Particle *p, float fraction) {
    int cx = (int)floorf(p->x), cy = (int)floorf(p->y);
    if (!particle_flies_through(w, cx, cy)) {
        return land(w, p, cx, cy); // something settled into its cell
    }
    float dx = p->vx * fraction, dy = p->vy * fraction;
    // Half-cell sub-steps, so no cell along the path is skipped.
    int n = (int)ceilf(PR_MAX(fabsf(dx), fabsf(dy)) * 2.0f);
    n = PR_MAX(n, 1);
    float x0 = p->x, y0 = p->y;
    for (int s = 1; s <= n; s++) {
        float nx = x0 + dx * (float)s / (float)n, ny = y0 + dy * (float)s / (float)n;
        int tx = (int)floorf(nx), ty = (int)floorf(ny);
        if (tx != cx || ty != cy) {
            // Blocked: the cell itself, or a diagonal step between two blocked cells.
            bool blocked = !particle_flies_through(w, tx, ty) ||
                           (tx != cx && ty != cy && !particle_flies_through(w, tx, cy) &&
                            !particle_flies_through(w, cx, ty));
            if (blocked) {
                p->x = (float)cx + 0.5f;
                p->y = (float)cy + 0.5f;
                return land(w, p, cx, cy);
            }
            cx = tx;
            cy = ty;
        }
        p->x = nx;
        p->y = ny;
    }
    return false;
}

void particles_update(World *w) {
    int i = 0;
    while (i < w->particle_count) {
        Particle *p = &w->particles[i];
        p->vy = PR_MIN(p->vy + PARTICLE_GRAVITY, PARTICLE_MAX_SPEED);
        p->vx = PR_CLAMP(p->vx, -PARTICLE_MAX_SPEED, PARTICLE_MAX_SPEED);
        if (particle_step(w, p, 1.0f)) {
            // Landed: the last particle takes its slot (and is updated next, in this loop).
            w->particles[i] = w->particles[--w->particle_count];
            continue;
        }
        i++;
    }
}
