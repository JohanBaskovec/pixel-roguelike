#include "sim/weapon.h"

#include "core/rng.h"
#include "sim/particle.h"

#include <math.h>

// Temperatures of a spent bullet and casing (K): warm from the shot.
#define BULLET_TEMP_K 400.0f
#define CASING_TEMP_K 370.0f

// Muzzle gas: the propellant (1.6 g, about what a 5.56 mm cartridge holds) leaves the muzzle as
// hot gas behind the bullet, at MUZZLE_K after expanding. By mass, single-base powder gives about
// 55% CO2 and CO, 25% water vapour, 18% nitrogen and 1-2% hydrogen. Here: 75% CO2 (carbon
// monoxide has no element; in the air, it and the hydrogen would burn in a "secondary flash",
// which a flash hider suppresses) and 25% nitrogen. Water vapour is left out (its mass split
// between the two): below 100 degC the model condenses steam, and humidity is not modelled. Each
// gas goes into a few cells in front of the muzzle, about MUZZLE_CELL_ATM atmospheres each; they
// expand from there, blown along the line of fire, and the current mixes them into the air (and
// their heat: turbulence, heat.c).
#define PROPELLANT_KG   1.6e-3f
#define MUZZLE_K        1500.0f
#define MUZZLE_CELL_ATM 6.0f
#define MUZZLE_REACH    6 // cells along the line of fire it is put in
#define MUZZLE_WIND     GAS_WIND_MAX

static const struct {
    ElementId gas;
    float fraction;
} k_muzzle_gas[] = {{ELEM_CO2, 0.75f}, {ELEM_NITROGEN, 0.25f}};
#define MUZZLE_GASES ((int)(sizeof(k_muzzle_gas) / sizeof(k_muzzle_gas[0])))

// Puts the muzzle gas in front of the muzzle at (x, y) (the bullet's first cell), firing along
// (ux, uy): cells along the line and either side of it, the gases taking turns.
static void muzzle_gas(World *w, float x, float y, float ux, float uy, uint64_t h) {
    int cand[3 * MUZZLE_REACH][2], nc = 0;
    for (int d = 0; d < MUZZLE_REACH; d++) {
        for (int l = 0; l < 3; l++) {
            float side = l == 0 ? 0.0f : (l == 1 ? 1.0f : -1.0f);
            int cx = (int)floorf(x + ux * (float)d - uy * side);
            int cy = (int)floorf(y + uy * (float)d + ux * side);
            bool dup = false;
            for (int k = 0; k < nc; k++) {
                dup |= cand[k][0] == cx && cand[k][1] == cy;
            }
            if (!dup && particle_passes(w, cx, cy)) {
                cand[nc][0] = cx;
                cand[nc][1] = cy;
                nc++;
            }
        }
    }
    if (nc == 0) {
        return; // fired into a wall: nowhere for it
    }
    int cells[MUZZLE_GASES];
    float share[MUZZLE_GASES];
    int left = 0;
    for (int g = 0; g < MUZZLE_GASES; g++) {
        float m = PROPELLANT_KG * k_muzzle_gas[g].fraction;
        float atm = m / gas_mass_at(k_muzzle_gas[g].gas, P_ATM, MUZZLE_K);
        cells[g] = PR_CLAMP((int)ceilf(atm / MUZZLE_CELL_ATM), 1, 4);
        share[g] = m / (float)cells[g];
        left += cells[g];
    }
    int next = 0;
    for (int round = 0; left > 0; round++) {
        for (int g = 0; g < MUZZLE_GASES; g++) {
            if (round >= cells[g]) {
                continue;
            }
            left--;
            // The next candidate that takes it (all of them tried, it is lost: no room).
            for (int tries = 0; tries < nc; tries++) {
                int k = next++ % nc;
                h = pr_splitmix64(h);
                if (world_gas_emit(w, cand[k][0], cand[k][1], k_muzzle_gas[g].gas, share[g],
                                   MUZZLE_K, (uint32_t)h)) {
                    break;
                }
            }
        }
    }
    int dir = gas_wind_dir_of(fabsf(ux) > 0.38f ? (ux > 0 ? 1 : -1) : 0,
                              fabsf(uy) > 0.38f ? (uy > 0 ? 1 : -1) : 0);
    for (int k = 0; k < nc; k++) {
        if (world_in_bounds(w, cand[k][0], cand[k][1])) {
            gas_push_wind(&w->cells[world_index(w, cand[k][0], cand[k][1])], dir, MUZZLE_WIND);
        }
    }
}

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

    // The bullet starts at the muzzle, RIFLE_LENGTH along the line of fire from the shoulder (well
    // clear of the body, even if the shooter moves forward before the bullet flies, at the start
    // of the next tick), or in front of a wall closer than that (the first cell it enters).
    float bx = sx, by = sy;
    for (int k = 0; k < 2 * RIFLE_LENGTH; k++) {
        float nx = bx + ux * 0.5f, ny = by + uy * 0.5f;
        int cx = (int)floorf(nx), cy = (int)floorf(ny);
        if (world_elem(w, cx, cy) != ELEM_BODY && !particle_flies_through(w, cx, cy)) {
            break;
        }
        bx = nx;
        by = ny;
    }
    Cell bullet = {ELEM_BULLET, 0, (uint8_t)(h >> 8), 0};
    Particle b = {bx, by, ux * RIFLE_BULLET_SPEED, uy * RIFLE_BULLET_SPEED, bullet, BULLET_TEMP_K};
    particle_add(w, &b);
    muzzle_gas(w, bx, by, ux, uy, h);

    // The casing is thrown up and back from the ejection port (or above the head if the port is
    // inside a wall). In flight it passes through bodies (particle.c). No room: the rifle keeps
    // it.
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
