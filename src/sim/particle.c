#include "sim/particle.h"

#include "core/rng.h"
#include "sim/edit.h"
#include "sim/enemy.h"
#include "sim/rigid.h"
#include "sim/weapon.h"

#include <math.h>

bool particle_add(World *w, const Particle *p) {
    if (w->particle_count >= PARTICLE_MAX) {
        return false;
    }
    w->particles[w->particle_count++] = *p;
    return true;
}

// Puts the particle's cell at (x, y), which must pass (void or gas). Gas there is squeezed into
// the gas around (spread out, as when water flows into gas). Returns false if that gas has
// nowhere to go.
static bool place(World *w, const Particle *p, int x, int y) {
    size_t i = world_index(w, x, y);
    ElementId here = w->cells[i].element;
    if (here != ELEM_VOID) {
        int32_t found[GAS_REGION_CELLS];
        int n = world_gas_region(w, x, y, x, y, GAS_REGION_RADIUS, GAS_REGION_CELLS, found);
        if (n == 0) {
            return false;
        }
        world_gas_spread_add(w, found, n, &w->gas[i], w->temp[i]);
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
    gas_mix_clear(&w->gas[i]);
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

// Can particle p fly through (x, y)? What any particle flies through (particle_flies_through),
// and, for powders in flight (casings, spent bullets, debris, kicked pixels), creatures' bodies:
// they never land on or against a body, so they never block one; they land on the ground, then
// lie there like sand.
static inline bool flies(const World *w, const Particle *p, int x, int y) {
    return particle_flies_through(w, x, y) ||
           (g_elem_behavior[p->cell.element] == BEHAVIOR_POWDER &&
            world_elem(w, x, y) == ELEM_BODY);
}

bool particle_step(World *w, Particle *p, float fraction) {
    int cx = (int)floorf(p->x), cy = (int)floorf(p->y);
    if (!flies(w, p, cx, cy)) {
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
            bool blocked = !flies(w, p, tx, ty) ||
                           (tx != cx && ty != cy && !flies(w, p, tx, cy) && !flies(w, p, cx, ty));
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

// ---- Bullets (M6a). docs/SIMULATION.md -> Rifle and bullets ----

// A bullet flies as a projectile while faster than BULLET_FAST (cells per tick, 30 m/s; it is
// fired at 300); slower, it is an ordinary particle that falls and lands as a pixel. (A spent
// bullet kicked by a walker, or knocked out of a pile by an impact, flies far slower: it must
// not dig like a fired one.)
#define BULLET_FAST 10.0f
// Current a bullet leaves in the gas it passes through (a speed, as in GAS_WIND_MAX units).
#define BULLET_WIND 8

// A bullet through wood loses this much speed a cell, and cuts a kerf (the cell and the one on
// either side across its path) out of it; the blow, BULLET_MOMENTUM (a 4 g bullet at 900 m/s,
// kg m/s), kicks the rigid body it hits.
#define BULLET_WOOD_RESISTANCE 30.0f
#define BULLET_MOMENTUM        3.6f

// Speed a bullet loses passing through one cell of `e` (cells per tick; it flies at 300).
// Negative: it cannot get through (it stops in front of it). Rock: it chips the cell and stops.
static float bullet_resistance(ElementId e) {
    switch (e) {
        case ELEM_DIRT:
            return 50.0f; // about 30 cm of packed earth stops it
        case ELEM_ROCK:
            return 1e9f;
        case ELEM_WOOD:
        case ELEM_BURNING_WOOD:
            return BULLET_WOOD_RESISTANCE; // pine: a bullet goes through about 60 cm
        default:
            break;
    }
    switch (g_elem_behavior[e]) {
        case BEHAVIOR_POWDER:
            return 50.0f; // sand or loose soil: about 30 cm (sandbags)
        case BEHAVIOR_LIQUID:
            return 15.0f * g_elem_defs[e].density / 1000.0f; // water: about 1 m
        default:
            return -1.0f; // iron, ice, bodies, the world's edge
    }
}

static float rand_unit(uint64_t *h) {
    *h = pr_splitmix64(*h);
    return (float)(*h >> 40) * (1.0f / 16777216.0f);
}

// The cell at (x, y), hit by a bullet flying along (ux, uy), is knocked out of the grid and thrown
// back out of the hole (debris: dirt loosened to soil, rock to rubble; nothing is destroyed). If
// the particle list is full it is only loosened, in place.
static void bullet_eject(World *w, int x, int y, float ux, float uy, uint64_t *h) {
    size_t i = world_index(w, x, y);
    Cell c = w->cells[i];
    c.element = elem_loosened(c.element);
    c.aux = 0;
    float back = 1.5f + 2.5f * rand_unit(h), side = 3.0f * rand_unit(h) - 1.5f;
    Particle d = {
        (float)x + 0.5f, (float)y + 0.5f, -ux * back - uy * side, -uy * back + ux * side - 0.5f, c,
        w->temp[i]};
    world_wake_rect(w, x - 1, y - 1, x + 1, y + 1);
    world_note_moved(w, x, y);
    if (!particle_add(w, &d)) {
        w->cells[i].element = c.element;
        w->cells[i].aux = 0;
        return;
    }
    w->cells[i] = (Cell){ELEM_VOID, (uint8_t)w->tick, w->cells[i].shade, 0};
    w->temp[i] = 0.0f;
    gas_mix_clear(&w->gas[i]);
    world_wake_heat(w, x - 1, y - 1, x + 1, y + 1);
}

// Erases the wood cell at (x, y) and the wood cells beside it across the bullet's path (the kerf):
// they become void, which the gas flows into. A rigid body that held them is cut.
static void bullet_cut_wood(World *w, int x, int y, float ux, float uy) {
    bool vertical = fabsf(uy) > fabsf(ux); // the kerf is across the path
    for (int k = -1; k <= 1; k++) {
        int cx = vertical ? x + k : x, cy = vertical ? y : y + k;
        ElementId e = world_elem(w, cx, cy);
        if (e != ELEM_WOOD && e != ELEM_BURNING_WOOD) {
            continue;
        }
        size_t i = world_index(w, cx, cy);
        w->cells[i] = (Cell){ELEM_VOID, (uint8_t)(w->tick - 1), w->cells[i].shade, 0};
        w->temp[i] = 0.0f;
        gas_mix_clear(&w->gas[i]);
        world_note_moved(w, cx, cy);
    }
    world_wake_rect(w, x - 2, y - 2, x + 2, y + 2);
    world_wake_heat(w, x - 2, y - 2, x + 2, y + 2);
}

// One tick of a fast bullet: it flies along its velocity (half-cell sub-steps), through void,
// gas and falling liquid, stirring the gas. Into dirt, powders and liquids it goes on, slowing by
// their resistance, knocking dirt and powder out as debris (and throwing up a splash where it
// enters a liquid). It stops in front of anything it cannot get through. Stopped (slower than
// BULLET_FAST), it is left to fall as an ordinary particle.
static void bullet_step(World *w, Particle *p) {
    float speed = sqrtf(p->vx * p->vx + p->vy * p->vy);
    float ux = p->vx / speed, uy = p->vy / speed;
    float ax = fabsf(ux), ay = fabsf(uy);
    int wind =
        gas_wind_dir_of(ax > 0.38f ? (ux > 0 ? 1 : -1) : 0, ay > 0.38f ? (uy > 0 ? 1 : -1) : 0);
    uint64_t h = pr_splitmix64(w->seed ^ pr_splitmix64(w->tick) ^ (uint64_t)(p->x * 4096.0f) ^
                               ((uint64_t)(p->y * 4096.0f) << 32));
    int cx = (int)floorf(p->x), cy = (int)floorf(p->y);
    bool in_liquid = g_elem_behavior[world_elem(w, cx, cy)] == BEHAVIOR_LIQUID;
    int n = (int)ceilf(speed * 2.0f);
    float x0 = p->x, y0 = p->y;
    for (int s = 1; s <= n && speed > BULLET_FAST; s++) {
        float nx = x0 + p->vx * (float)s / (float)n, ny = y0 + p->vy * (float)s / (float)n;
        int tx = (int)floorf(nx), ty = (int)floorf(ny);
        if (tx != cx || ty != cy) {
            ElementId e = world_elem(w, tx, ty);
            if (particle_flies_through(w, tx, ty)) {
                if (g_elem_behavior[e] == BEHAVIOR_GAS) {
                    gas_push_wind(&w->cells[world_index(w, tx, ty)], wind, BULLET_WIND);
                    world_wake_rect(w, tx, ty, tx, ty);
                }
                in_liquid = false;
            } else {
                if (e == ELEM_BODY) {
                    // It hits a creature: damage and blood (the player is not hurt by its own
                    // bullets), and it stops.
                    int ci = creature_at(w, tx, ty);
                    if (ci >= 0 && w->creatures[ci].kind != KIND_PLAYER) {
                        creature_hit(w, &w->creatures[ci], RIFLE_DAMAGE, cx, cy, ux, uy);
                    }
                    speed = 0.0f;
                    break;
                }
                float r = bullet_resistance(e);
                if (r < 0.0f) {
                    speed = 0.0f; // stops in front of it
                    break;
                }
                speed -= r;
                if (g_elem_behavior[e] == BEHAVIOR_LIQUID) {
                    if (!in_liquid) {
                        bullet_eject(w, tx, ty, ux, uy, &h); // the splash where it goes in
                    }
                    in_liquid = true;
                } else if (e == ELEM_WOOD || e == ELEM_BURNING_WOOD) {
                    rigid_hit(w, tx, ty, ux, uy, BULLET_MOMENTUM);
                    bullet_cut_wood(w, tx, ty, ux, uy);
                    in_liquid = false;
                } else {
                    bullet_eject(w, tx, ty, ux, uy, &h);
                    sim_loosen_small_clumps(w, tx - 1, ty - 1, tx + 1, ty + 1);
                    in_liquid = false;
                }
                if (e == ELEM_ROCK) {
                    break; // chipped it, and stopped: it falls from in front of the chip
                }
            }
            cx = tx;
            cy = ty;
        }
        p->x = nx;
        p->y = ny;
    }
    if (speed <= BULLET_FAST) {
        // Stopped: from the middle of the last cell it reached, it drops (bouncing back a little).
        p->x = (float)cx + 0.5f;
        p->y = (float)cy + 0.5f;
        p->vx = -ux * 0.3f;
        p->vy = 0.0f;
        return;
    }
    p->vx = ux * speed;
    p->vy = uy * speed;
}

void particles_update(World *w) {
    int i = 0;
    while (i < w->particle_count) {
        Particle *p = &w->particles[i];
        if (p->cell.element == ELEM_BULLET && fabsf(p->vx) + fabsf(p->vy) > BULLET_FAST) {
            p->vy += PARTICLE_GRAVITY;
            bullet_step(w, p);
            i++;
            continue;
        }
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
