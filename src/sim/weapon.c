#include "sim/weapon.h"

#include "core/rng.h"
#include "sim/particle.h"

#include <math.h>

// Temperatures of a spent bullet and casing (K): warm from the shot.
#define BULLET_TEMP_K 400.0f
#define CASING_TEMP_K 370.0f

static float rand_unit(uint64_t *h) {
    *h = pr_splitmix64(*h);
    return (float)(*h >> 40) * (1.0f / 16777216.0f);
}

void rifle_shoulder(const Creature *c, float *x, float *y) {
    *x = (float)c->x + (float)c->w * 0.5f;
    *y = (float)c->y + (float)RIFLE_SHOULDER + 0.5f;
}

static void fire(World *w, Rifle *r, const Creature *c, float aim_x, float aim_y) {
    uint64_t h = pr_splitmix64(w->seed ^ 0x5407ull ^ ((uint64_t)c->id << 40) ^ r->shots);
    float sx, sy;
    rifle_shoulder(c, &sx, &sy);
    float dx = aim_x - sx, dy = aim_y - sy;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) {
        dx = (float)c->facing;
        dy = 0.0f;
        len = 1.0f;
    }
    dx /= len;
    dy /= len;
    float a = RIFLE_SPREAD * (2.0f * rand_unit(&h) - 1.0f);
    float ca = cosf(a), sa = sinf(a);
    float ux = dx * ca - dy * sa, uy = dx * sa + dy * ca;

    // The bullet leaves the body along the line of fire: it starts at the last cell of the body
    // on that line (a cell it does not check), so the first cell it enters is outside it.
    float bx = sx, by = sy;
    for (int k = 0; k < 4 * (CREATURE_MAX_W + CREATURE_MAX_H); k++) {
        float nx = bx + ux * 0.5f, ny = by + uy * 0.5f;
        if (world_elem(w, (int)floorf(nx), (int)floorf(ny)) != ELEM_BODY) {
            break;
        }
        bx = nx;
        by = ny;
    }
    Cell bullet = {ELEM_BULLET, 0, (uint8_t)(h >> 8), 0};
    Particle b = {bx, by, ux * RIFLE_BULLET_SPEED, uy * RIFLE_BULLET_SPEED, bullet, BULLET_TEMP_K};
    particle_add(w, &b);

    // The casing is thrown up and back from the ejection port (or above the head if the port is
    // inside a wall). No room: the rifle keeps it.
    int px = (int)floorf(sx + dx * RIFLE_PORT), py = (int)floorf(sy + dy * RIFLE_PORT);
    if (!particle_passes(w, px, py)) {
        px = c->x + c->w / 2;
        py = c->y - 1;
    }
    if (particle_passes(w, px, py)) {
        float back = dx >= 0.0f ? -1.0f : 1.0f;
        Cell casing = {ELEM_CASING, 0, (uint8_t)(h >> 16), 0};
        Particle k = {(float)px + 0.5f,
                      (float)py + 0.5f,
                      back * (0.5f + 0.7f * rand_unit(&h)),
                      -(0.8f + 0.8f * rand_unit(&h)),
                      casing,
                      CASING_TEMP_K};
        particle_add(w, &k);
    }
    r->shots++;
}

bool rifle_update(World *w, Rifle *r, int shooter, float aim_x, float aim_y, bool trigger) {
    if (r->cooldown > 0) {
        r->cooldown--;
    }
    if (!trigger || r->cooldown > 0 || shooter < 0 || shooter >= w->creature_count) {
        return false;
    }
    const Creature *c = &w->creatures[shooter];
    if (c->state == CREATURE_DEAD) {
        return false;
    }
    fire(w, r, c, aim_x, aim_y);
    r->cooldown = RIFLE_INTERVAL;
    return true;
}
