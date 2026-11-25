#include "sim/enemy.h"

#include "core/rng.h"
#include "sim/particle.h"
#include "sim/path.h"
#include "sim/rigid.h"

#include <math.h>

#define BLOOD_TEMP_K T_ROOM
#define BLEED_TICKS  5    // ticks between droplets of a bleeding body
#define BLEED_PER_CELL 1.5f // droplets a dead body bleeds, per cell of its body

static float rand_unit(uint64_t *h) {
    *h = pr_splitmix64(*h);
    return (float)(*h >> 40) * (1.0f / 16777216.0f);
}

int enemy_spawn(World *w, int kind, int x, int y) {
    PR_ASSERT(kind == KIND_WALKER || kind == KIND_FLYER);
    bool walker = kind == KIND_WALKER;
    int id = creature_spawn_sized(w, x, y, walker ? WALKER_W : FLYER_W, walker ? WALKER_H : FLYER_H);
    if (id < 0) {
        return -1;
    }
    Creature *c = &w->creatures[id];
    // Monsters give off no heat: their body starts at the air's temperature.
    for (int r = 0; r < c->h; r++) {
        for (int k = 0; k < c->w; k++) {
            w->temp[world_index(w, c->x + k, c->y + r)] = T_ROOM;
        }
    }
    c->kind = (uint8_t)kind;
    c->hp = c->hp_max = walker ? WALKER_HP : FLYER_HP;
    c->speed = walker ? WALKER_SPEED : FLYER_SPEED;
    c->wander = 1;
    return id;
}

int creature_at(const World *w, int x, int y) {
    for (int i = 0; i < w->creature_count; i++) {
        const Creature *c = &w->creatures[i];
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) {
            return i;
        }
    }
    return -1;
}

void blood_splash(World *w, int x, int y, float ux, float uy, int n, uint64_t seed) {
    if (!particle_passes(w, x, y)) {
        return;
    }
    uint64_t h = pr_splitmix64(seed ^ 0xB100Dull ^ (w->tick << 20));
    for (int k = 0; k < n; k++) {
        float back = 0.4f + 2.2f * rand_unit(&h), side = 2.4f * rand_unit(&h) - 1.2f;
        Cell cell = {ELEM_BLOOD, 0, (uint8_t)(h >> 8), 0};
        Particle d = {(float)x + 0.5f,
                      (float)y + 0.5f,
                      -ux * back - uy * side,
                      -uy * back + ux * side - 0.4f - 0.8f * rand_unit(&h),
                      cell,
                      BLOOD_TEMP_K};
        if (!particle_add(w, &d)) {
            return;
        }
    }
}

static bool is_monster(const Creature *c) {
    return c->kind == KIND_WALKER || c->kind == KIND_FLYER;
}

int monster_pixels(const World *w, const Creature *c) {
    int n = 0;
    for (int r = 0; r < c->h; r++) {
        for (int k = 0; k < c->w; k++) {
            n += world_elem(w, c->x + k, c->y + r) == ELEM_BODY;
        }
    }
    return n;
}

bool monster_lose_pixel(World *w, Creature *c, int x, int y) {
    int best_x = 0, best_y = 0, best_d = 1 << 30;
    for (int r = 0; r < c->h; r++) {
        for (int k = 0; k < c->w; k++) {
            int px = c->x + k, py = c->y + r;
            if (world_elem(w, px, py) != ELEM_BODY) {
                continue;
            }
            int d = (px - x) * (px - x) + (py - y) * (py - y);
            if (d < best_d) {
                best_d = d;
                best_x = px;
                best_y = py;
            }
        }
    }
    if (best_d == 1 << 30) {
        return false;
    }
    size_t i = world_index(w, best_x, best_y);
    w->cells[i] = (Cell){ELEM_VOID, (uint8_t)(w->tick - 1), w->cells[i].shade, 0};
    w->temp[i] = 0.0f;
    gas_mix_clear(&w->gas[i]);
    world_note_moved(w, best_x, best_y);
    world_wake_rect(w, best_x - 2, best_y - 2, best_x + 2, best_y + 2);
    world_wake_heat(w, best_x - 1, best_y - 1, best_x + 1, best_y + 1);
    c->px_lost++;
    return true;
}

// Is what is left of the monster's body in one piece (4-neighbour)?
static bool body_in_one_piece(const World *w, const Creature *c) {
    uint8_t seen[CREATURE_MAX_W * CREATURE_MAX_H] = {0};
    int stack[CREATURE_MAX_W * CREATURE_MAX_H], top = 0, total = 0, reached = 0;
    for (int r = 0; r < c->h; r++) {
        for (int k = 0; k < c->w; k++) {
            if (world_elem(w, c->x + k, c->y + r) == ELEM_BODY) {
                total++;
                if (top == 0 && reached == 0) {
                    stack[top++] = r * c->w + k;
                    seen[r * c->w + k] = 1;
                }
            }
        }
    }
    while (top > 0) {
        int p = stack[--top], k = p % c->w, r = p / c->w;
        reached++;
        static const int8_t d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int q = 0; q < 4; q++) {
            int nk = k + d4[q][0], nr = r + d4[q][1];
            if (nk < 0 || nr < 0 || nk >= c->w || nr >= c->h || seen[nr * c->w + nk] ||
                world_elem(w, c->x + nk, c->y + nr) != ELEM_BODY) {
                continue;
            }
            seen[nr * c->w + nk] = 1;
            stack[top++] = nr * c->w + nk;
        }
    }
    return reached == total;
}

void monster_die(World *w, Creature *c) {
    if (c->corpse) {
        return;
    }
    int32_t cells[CREATURE_MAX_W * CREATURE_MAX_H];
    int n = 0;
    for (int r = 0; r < c->h; r++) {
        for (int k = 0; k < c->w; k++) {
            if (world_elem(w, c->x + k, c->y + r) == ELEM_BODY) {
                cells[n++] = (int32_t)world_index(w, c->x + k, c->y + r);
            }
        }
    }
    ElementId material = c->kind == KIND_WALKER ? ELEM_WALKER_CORPSE : ELEM_FLYER_CORPSE;
    world_sound(w, SOUND_MONSTER_DIE, c->x + c->w / 2, c->y + c->h / 2, 0.9f);
    c->hp = 0.0f;
    c->state = CREATURE_DEAD;
    c->walk = 0;
    c->jump = 0;
    if (n > 0 && rigid_spawn_from_cells(w, cells, n, material, c->vx, c->vy,
                                        BLEED_PER_CELL * (float)n) < 0) {
        // No room for a body: the cells are left as flesh that does not move.
        for (int k = 0; k < n; k++) {
            w->cells[cells[k]].element = material;
        }
    }
    c->corpse = 1; // this entry is inert from now on: the body is the rigid body's
    c->w = c->h = 0;
}

void creature_hit(World *w, Creature *c, float hp, int x, int y, float ux, float uy, int bx,
                  int by) {
    if (c->state == CREATURE_DEAD) {
        blood_splash(w, x, y, ux, uy, 3, c->id * 7919u + (uint64_t)w->tick);
        return;
    }
    float lost = PR_MIN(hp, c->hp); // hit points actually lost (the last blow is not all of it)
    c->hp -= hp;
    c->hit_flash = 6;
    world_sound(w, c->kind == KIND_PLAYER ? SOUND_PLAYER_HURT : SOUND_IMPACT_FLESH, c->x + c->w / 2,
                c->y + c->h / 2, 0.6f + PR_MIN(hp / 40.0f, 0.4f));
    blood_splash(w, x, y, ux, uy, 5 + (int)(hp / 3.0f), c->id * 31u + (uint64_t)w->tick);
    if (is_monster(c)) {
        int total = c->w * c->h;
        if (bx >= 0) {
            // Pixels owed: the monster's width over its hit points, for each hit point lost.
            c->px_debt += lost * (float)c->w / c->hp_max;
            while (c->px_debt >= 1.0f && c->px_lost < c->w) {
                monster_lose_pixel(w, c, bx, by);
                c->px_debt -= 1.0f;
            }
        }
        if (c->hp <= 0.0f || 2 * monster_pixels(w, c) < total || !body_in_one_piece(w, c)) {
            monster_die(w, c);
        }
        return;
    }
    if (c->hp <= 0.0f) {
        c->hp = 0.0f;
        world_sound(w, SOUND_PLAYER_DIE, c->x + c->w / 2, c->y + c->h / 2, 1.0f);
        c->state = CREATURE_DEAD;
        c->walk = 0;
        c->bleed = BLEED_PER_CELL * (float)(c->w * c->h);
    }
}

void creature_damage_thermal(World *w, Creature *c, float hp) {
    if (c->state == CREATURE_DEAD) {
        return;
    }
    c->hp -= hp;
    if ((w->tick + c->id) % 20 == 0) {
        c->hit_flash = 4;
        if (!is_monster(c)) {
            world_sound(w, SOUND_PLAYER_HURT, c->x + c->w / 2, c->y + c->h / 2, 0.4f);
        }
    }
    if (c->hp > 0.0f) {
        return;
    }
    c->hp = 0.0f;
    if (is_monster(c)) {
        monster_die(w, c);
        return;
    }
    world_sound(w, SOUND_PLAYER_DIE, c->x + c->w / 2, c->y + c->h / 2, 1.0f);
    c->state = CREATURE_DEAD;
    c->walk = 0;
    c->bleed = BLEED_PER_CELL * (float)(c->w * c->h);
}

void enemy_bleed(World *w, Creature *c) {
    if (c->bleed <= 0.0f || (w->tick + c->id) % BLEED_TICKS != 0) {
        return;
    }
    uint64_t h = pr_splitmix64(w->seed ^ (w->tick * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)c->id << 24));
    // From a cell beside the body, at a random height, dripping out and down.
    bool left = rand_unit(&h) < 0.5f;
    int y = c->y + (int)(rand_unit(&h) * (float)c->h);
    y = PR_MIN(y, c->y + c->h - 1);
    int x = left ? c->x - 1 : c->x + c->w;
    if (!particle_passes(w, x, y)) {
        return;
    }
    Cell cell = {ELEM_BLOOD, 0, (uint8_t)(h >> 8), 0};
    float out = left ? -1.0f : 1.0f;
    Particle d = {(float)x + 0.5f, (float)y + 0.5f, out * (0.1f + 0.4f * rand_unit(&h)),
                  -0.1f * rand_unit(&h), cell, BLOOD_TEMP_K};
    if (particle_add(w, &d)) {
        c->bleed -= 1.0f;
    }
}

// Is the line between two points free of anything solid? (Void, gas, liquid and bodies do not
// block the view.)
static bool in_sight(const World *w, float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0;
    int n = (int)ceilf(PR_MAX(fabsf(dx), fabsf(dy)));
    for (int i = 1; i < n; i++) {
        float t = (float)i / (float)n;
        ElementId e = world_elem(w, (int)floorf(x0 + dx * t), (int)floorf(y0 + dy * t));
        uint8_t b = g_elem_behavior[e];
        if (e != ELEM_BODY && b != BEHAVIOR_NONE && b != BEHAVIOR_GAS && b != BEHAVIOR_LIQUID) {
            return false;
        }
    }
    return true;
}

// Are the two bodies touching (at most a cell apart)?
static bool touching(const Creature *a, const Creature *b) {
    int gx = PR_MAX(a->x - (b->x + b->w), b->x - (a->x + a->w));
    int gy = PR_MAX(a->y - (b->y + b->h), b->y - (a->y + a->h));
    return gx <= 1 && gy <= 1;
}

static float approach(float v, float target, float step) {
    return v + PR_CLAMP(target - v, -step, step);
}

void enemy_think(World *w, Creature *c) {
    bool walker = c->kind == KIND_WALKER;
    Creature *p = NULL;
    for (int i = 0; i < w->creature_count; i++) {
        if (w->creatures[i].kind == KIND_PLAYER && w->creatures[i].state != CREATURE_DEAD) {
            p = &w->creatures[i];
            break;
        }
    }
    float cx = (float)c->x + (float)c->w * 0.5f, cy = (float)c->y + (float)c->h * 0.5f;
    float dx = 0.0f, dy = 0.0f;
    if (p) {
        dx = (float)p->x + (float)p->w * 0.5f - cx;
        dy = (float)p->y + (float)p->h * 0.5f - cy;
    }
    // It notices the player when it sees it on the screen, and chases until it is off it.
    bool on_screen = p && fabsf(dx) <= (float)w->view_w * 0.5f && fabsf(dy) <= (float)w->view_h * 0.5f;
    if (!on_screen) {
        c->alert = 0;
    } else if (!c->alert && in_sight(w, cx, cy, cx + dx, cy + dy)) {
        c->alert = 1;
    }
    bool chase = c->alert;
    // In sight it goes straight for the player; otherwise it follows the route (path.h).
    bool sight = on_screen && in_sight(w, cx, cy, cx + dx, cy + dy);
    bool routed_ok = chase && !sight && path_update(w, (int)(cx + dx), (int)(cy + dy));
    if (c->attack_cd > 0) {
        c->attack_cd--;
    }
    c->wander = !chase; // wandering picks `walk` at random (creature.c)
    if (walker) {
        if (chase) {
            int pw = 0, pj = 0;
            bool routed = routed_ok && path_walker_step(w, (int)cx, (int)cy, &pw, &pj);
            if (routed) {
                c->walk = (int8_t)pw;
            } else {
                c->walk = fabsf(dx) < 2.0f ? 0 : (int8_t)(dx > 0.0f ? 1 : -1);
            }
            // Jump when the route goes up a ledge, the player is higher (straight chase), or
            // something blocks the way at foot level.
            int ahead = c->walk > 0 ? c->x + c->w : c->x - 1;
            bool blocked = c->walk != 0 && !particle_passes(w, ahead, c->y + c->h - 1);
            if (c->grounded && (blocked || pj || (!routed && dy < -(float)c->h * 1.5f))) {
                c->jump = 1;
            }
        }
    } else {
        float tvx, tvy;
        if (chase) {
            float gx = dx, gy = dy, k = 1.0f;
            float wx, wy;
            if (routed_ok && path_flyer_step(w, (int)cx, (int)cy, &wx, &wy)) {
                gx = wx - cx; // the next waypoint of the route
                gy = wy - cy;
            } else if (dx * dx + dy * dy < 36.0f) {
                k = 0.2f; // slow down when close
            }
            float len = sqrtf(gx * gx + gy * gy);
            len = PR_MAX(len, 1.0f);
            tvx = gx / len * c->speed * k;
            tvy = gy / len * c->speed * k;
        } else {
            // Idle: drift sideways with the wandering direction, bobbing a little.
            tvx = (float)c->walk * c->speed * 0.4f;
            tvy = 0.15f * sinf((float)(w->tick + c->id * 37u) * 0.05f);
        }
        c->vx = approach(c->vx, tvx, 0.06f);
        c->vy = approach(c->vy, tvy, 0.06f);
    }
    if (p && c->attack_cd == 0 && touching(c, p)) {
        float len = sqrtf(dx * dx + dy * dy);
        len = PR_MAX(len, 1.0f);
        float ux = dx / len, uy = dy / len; // the bite comes from the monster toward the player
        // Blood leaves the player's body on the monster's side, at the bite's height.
        int bx = dx > 0.0f ? p->x - 1 : p->x + p->w;
        int by = PR_CLAMP((int)cy, p->y, p->y + p->h - 1);
        creature_hit(w, p, walker ? WALKER_BITE : FLYER_BITE, bx, by, ux, uy, -1, -1);
        c->attack_cd = BITE_TICKS;
        world_sound(w, SOUND_BITE, c->x + c->w / 2, c->y + c->h / 2, 0.8f);
    }
}
