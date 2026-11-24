#include "sim/weapon.h"

#include "core/rng.h"
#include "sim/particle.h"

#include <math.h>

// Temperatures of a fired bullet and its casing (K). Real ones leave warm (about 100 degC), but a
// 4 g bullet then holds about 50 J and a 10 g casing about 300 J above the air's temperature. A
// cell stands for a 5 cm cube: a bullet cell at 400 K would hold 19 kJ (1.4 kg of lead), 350
// times too much, and heat a trail of air all the way down as it falls (a burst fired at the sky
// kept the heat pass running over 700 chunks). So both start at room temperature: the heat they
// really carry is negligible. (The shot's heat is in the muzzle gas, which holds the real amount.)
#define BULLET_TEMP_K T_ROOM
#define CASING_TEMP_K T_ROOM

// Muzzle gas: the propellant (1.6 g, about what a 5.56 mm cartridge holds) leaves the muzzle as
// hot gas behind the bullet, at MUZZLE_K after expanding. By mass, single-base powder gives about
// 55% CO2 and CO, 25% water vapour, 18% nitrogen and 1-2% hydrogen. Here: 55% CO2 (carbon
// monoxide has no species; in the air, it and the hydrogen would burn in a "secondary flash",
// which a flash hider suppresses), 27% water vapour and 18% nitrogen. It is mixed into the gas
// of the first MUZZLE_REACH cells along the line of fire (in equal shares), which it heats and
// pressurises; it expands from there, blown along the line of fire, and its current mixes it and
// its heat into the air (turbulence, D-034).
#define PROPELLANT_KG 1.6e-3f
#define MUZZLE_K      1500.0f
#define MUZZLE_REACH  6 // cells along the line of fire it is put in
#define MUZZLE_WIND   GAS_WIND_MAX

// Puts the muzzle gas in front of the muzzle at (x, y) (the bullet's first cell), firing along
// (ux, uy), and gives the gas there (along the line and either side of it) a current.
static void muzzle_gas(World *w, float x, float y, float ux, float uy, uint64_t h) {
    GasMix g;
    gas_mix_clear(&g);
    g.m[GAS_CO2] = PROPELLANT_KG * 0.55f;
    g.m[GAS_H2O] = PROPELLANT_KG * 0.27f;
    g.m[GAS_N2] = PROPELLANT_KG * 0.18f;
    int cells[MUZZLE_REACH][2], nc = 0;
    for (int d = 0; d < MUZZLE_REACH; d++) {
        int cx = (int)floorf(x + ux * (float)d), cy = (int)floorf(y + uy * (float)d);
        if (!particle_passes(w, cx, cy)) {
            break; // a wall
        }
        if (nc == 0 || cells[nc - 1][0] != cx || cells[nc - 1][1] != cy) {
            cells[nc][0] = cx;
            cells[nc][1] = cy;
            nc++;
        }
    }
    if (nc == 0) {
        return; // fired into a wall: nowhere for it
    }
    gas_mix_scale(&g, 1.0f / (float)nc);
    for (int k = 0; k < nc; k++) {
        h = pr_splitmix64(h);
        world_gas_emit(w, cells[k][0], cells[k][1], &g, MUZZLE_K, (uint32_t)h);
    }
    int dir = gas_wind_dir_of(fabsf(ux) > 0.38f ? (ux > 0 ? 1 : -1) : 0,
                              fabsf(uy) > 0.38f ? (uy > 0 ? 1 : -1) : 0);
    for (int k = 0; k < nc; k++) {
        for (int side = -1; side <= 1; side++) {
            int cx = (int)floorf((float)cells[k][0] + 0.5f - uy * (float)side);
            int cy = (int)floorf((float)cells[k][1] + 0.5f + ux * (float)side);
            if (world_in_bounds(w, cx, cy)) {
                gas_push_wind(&w->cells[world_index(w, cx, cy)], dir, MUZZLE_WIND);
            }
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
