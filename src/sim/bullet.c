#include "sim/bullet.h"

#include "core/rng.h"
#include "sim/particle.h"

#include <math.h>
#include <string.h>

bool bullet_element_is_solid(ElementId e) {
    uint8_t b = g_elem_behavior[e];
    return b == BEHAVIOR_STATIC || b == BEHAVIOR_POWDER;
}

bool bullet_element_allowed(ElementId e) {
    if (e >= ELEM_COUNT || e == ELEM_VOID || e == ELEM_BODY || e == ELEM_BURNING_WOOD ||
        e == ELEM_GAS || e == ELEM_WALKER_CORPSE || e == ELEM_FLYER_CORPSE) {
        return false;
    }
    return true;
}

void bullet_design_clear(BulletDesign *d) {
    memset(d, 0, sizeof(*d));
}

void bullet_design_iron(BulletDesign *d) {
    bullet_design_clear(d);
    d->px[0] = (BulletPixel){BULLET_MATTER, ELEM_IRON, RELEASE_IMPACT, 0.0f, 0.0f, 0.0f};
}

int bullet_design_pixels(const BulletDesign *d) {
    int n = 0;
    for (int i = 0; i < BULLET_PIXELS; i++) {
        n += d->px[i].kind != BULLET_EMPTY;
    }
    return n;
}

bool bullet_design_connected(const BulletDesign *d) {
    int total = bullet_design_pixels(d);
    if (total == 0) {
        return false;
    }
    uint8_t seen[BULLET_PIXELS] = {0};
    int stack[BULLET_PIXELS], top = 0, reached = 0;
    for (int i = 0; i < BULLET_PIXELS && top == 0; i++) {
        if (d->px[i].kind != BULLET_EMPTY) {
            stack[top++] = i;
            seen[i] = 1;
        }
    }
    while (top > 0) {
        int p = stack[--top], x = p % BULLET_DIM, y = p / BULLET_DIM;
        reached++;
        static const int8_t d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int k = 0; k < 4; k++) {
            int nx = x + d4[k][0], ny = y + d4[k][1];
            if (nx < 0 || ny < 0 || nx >= BULLET_DIM || ny >= BULLET_DIM) {
                continue;
            }
            int q = ny * BULLET_DIM + nx;
            if (!seen[q] && d->px[q].kind != BULLET_EMPTY) {
                seen[q] = 1;
                stack[top++] = q;
            }
        }
    }
    return reached == total;
}

float bullet_design_damage(const BulletDesign *d) {
    float dmg = 0.0f;
    for (int i = 0; i < BULLET_PIXELS; i++) {
        const BulletPixel *p = &d->px[i];
        if (p->kind == BULLET_MATTER && bullet_element_is_solid(p->element)) {
            float rel = g_elem_defs[p->element].density / g_elem_defs[ELEM_IRON].density;
            dmg += BULLET_SOLID_DAMAGE * PR_CLAMP(rel, 0.1f, 1.5f);
        }
    }
    return dmg;
}

bool bullet_design_has_payload(const BulletDesign *d) {
    for (int i = 0; i < BULLET_PIXELS; i++) {
        const BulletPixel *p = &d->px[i];
        if (p->kind == BULLET_HEAT ||
            (p->kind == BULLET_MATTER && !bullet_element_is_solid(p->element))) {
            return true;
        }
    }
    return false;
}

bool bullet_design_has_solid(const BulletDesign *d) {
    for (int i = 0; i < BULLET_PIXELS; i++) {
        if (d->px[i].kind == BULLET_MATTER && bullet_element_is_solid(d->px[i].element)) {
            return true;
        }
    }
    return false;
}

// ---- Payloads ----

int payload_start(World *w, const BulletDesign *d) {
    for (int id = 1; id <= PAYLOAD_MAX; id++) {
        Payload *p = &w->payloads[id];
        if (!p->active) {
            p->active = 1;
            p->design = *d;
            for (int i = 0; i < BULLET_PIXELS; i++) {
                p->left[i] = 1.0f;
            }
            return id;
        }
    }
    return 0;
}

float payload_damage(const World *w, int id) {
    return bullet_design_damage(&w->payloads[id].design);
}

void payloads_reset(World *w) {
    if (w->payloads) {
        memset(w->payloads, 0, sizeof(Payload) * (PAYLOAD_MAX + 1));
    }
}

// Releases the fraction `f` (of the whole pixel) of pixel i at (x, y).
static void release(World *w, Payload *pl, int i, float f, int x, int y, int hx, int hy,
                    float ux, float uy, uint64_t *h) {
    const BulletPixel *p = &pl->design.px[i];
    f = PR_MIN(f, pl->left[i]);
    if (f <= 0.0f) {
        return;
    }
    pl->left[i] -= f;
    *h = pr_splitmix64(*h);
    if (p->kind == BULLET_HEAT) {
        // Toward the temperature, by the fraction, in the cell and (at the impact) around it.
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int cx = hx + dx, cy = hy + dy;
                bool around = hx != x || hy != y;
                if (!around) {
                    cx = x + dx;
                    cy = y + dy;
                }
                if (!world_in_bounds(w, cx, cy)) {
                    continue;
                }
                size_t k = world_index(w, cx, cy);
                if (g_elem_cap[w->cells[k].element] <= 0.0f) {
                    continue;
                }
                w->temp[k] += (p->temp_k - w->temp[k]) * f;
            }
        }
        world_wake_heat(w, hx - 2, hy - 2, hx + 2, hy + 2);
        world_wake_rect(w, hx - 2, hy - 2, hx + 2, hy + 2);
        return;
    }
    ElementId e = p->element;
    if (elem_species(e) >= 0) {
        float t = g_elem_defs[e].spawn_temp;
        float m = gas_mass_at(e, p->kpa * 1000.0f, t) * f;
        GasMix mix = gas_mix_pure(elem_species(e), m);
        world_gas_emit(w, x, y, &mix, t, (uint32_t)(*h >> 20));
        return;
    }
    if (g_elem_behavior[e] == BEHAVIOR_LIQUID) {
        // A liquid pixel is one cell: it comes out, whole, with chance f (all of it, at an impact).
        float u = (float)(*h >> 40) * (1.0f / 16777216.0f);
        if (u >= f && f < 1.0f) {
            pl->left[i] += f; // not this time
            return;
        }
        pl->left[i] = 0.0f;
        Cell cell = {e, 0, (uint8_t)(*h >> 8), 0};
        float side = ((float)((*h >> 12) & 255) / 255.0f - 0.5f) * 1.2f;
        Particle d = {(float)x + 0.5f, (float)y + 0.5f, -ux * 0.6f - uy * side,
                      -uy * 0.6f + ux * side - 0.3f, cell, g_elem_defs[e].spawn_temp};
        particle_add(w, &d);
    }
}

void payload_trail(World *w, int id, int x, int y) {
    Payload *pl = &w->payloads[id];
    uint64_t h = pr_splitmix64(w->seed ^ (w->tick << 12) ^ (uint64_t)(uint32_t)x * 31u ^
                               ((uint64_t)(uint32_t)y << 32) ^ (uint64_t)id);
    for (int i = 0; i < BULLET_PIXELS; i++) {
        const BulletPixel *p = &pl->design.px[i];
        if (p->kind == BULLET_EMPTY || p->release != RELEASE_GRADUAL || pl->left[i] <= 0.0f ||
            (p->kind == BULLET_MATTER && bullet_element_is_solid(p->element))) {
            continue;
        }
        release(w, pl, i, p->percent / 100.0f, x, y, x, y, 0.0f, 0.0f, &h);
    }
}

bool payload_impact(World *w, int id, int x, int y, int hx, int hy, float ux, float uy) {
    Payload *pl = &w->payloads[id];
    uint64_t h = pr_splitmix64(w->seed ^ (w->tick << 8) ^ (uint64_t)id ^ 0x1F4Cull);
    for (int i = 0; i < BULLET_PIXELS; i++) {
        const BulletPixel *p = &pl->design.px[i];
        if (p->kind == BULLET_EMPTY || pl->left[i] <= 0.0f ||
            (p->kind == BULLET_MATTER && bullet_element_is_solid(p->element))) {
            continue;
        }
        release(w, pl, i, pl->left[i], x, y, hx, hy, ux, uy, &h);
    }
    int loose = 0; // pixels that let go of something now
    for (int i = 0; i < BULLET_PIXELS; i++) {
        const BulletPixel *p = &pl->design.px[i];
        loose += p->kind != BULLET_EMPTY && pl->left[i] > 0.0f &&
                 !(p->kind == BULLET_MATTER && bullet_element_is_solid(p->element));
    }
    if (loose > 0) {
        world_sound(w, SOUND_BLAST, x, y, 0.35f + 0.65f * (float)loose / 8.0f);
    }
    bool solid = bullet_design_has_solid(&pl->design);
    pl->active = 0;
    return solid;
}
