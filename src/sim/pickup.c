#include "sim/pickup.h"

#include "sim/particle.h"

int pickup_add(World *w, int kind, int cx, int bottom) {
    if (w->pickup_count >= PICKUP_MAX) {
        return -1;
    }
    Pickup *p = &w->pickups[w->pickup_count];
    *p = (Pickup){cx - PICKUP_W / 2, bottom - PICKUP_H + 1, PICKUP_W, PICKUP_H, (uint8_t)kind, 0};
    return w->pickup_count++;
}

void pickups_update(World *w) {
    for (int i = 0; i < w->pickup_count; i++) {
        Pickup *p = &w->pickups[i];
        if (p->taken) {
            continue;
        }
        int cx = PR_CLAMP(p->x + p->w / 2, 0, w->width - 1) >> CHUNK_SHIFT;
        int cy = PR_CLAMP(p->y + p->h / 2, 0, w->height - 1) >> CHUNK_SHIFT;
        if (!world_chunk(w, cx, cy)->active) {
            continue;
        }
        bool open = true;
        for (int k = 0; k < p->w && open; k++) {
            open = particle_passes(w, p->x + k, p->y + p->h);
        }
        if (open) {
            p->y++;
        }
    }
}

int pickups_collect(World *w, int who) {
    if (who < 0 || who >= w->creature_count) {
        return 0;
    }
    Creature *c = &w->creatures[who];
    if (c->state == CREATURE_DEAD) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < w->pickup_count; i++) {
        Pickup *p = &w->pickups[i];
        if (p->taken || p->x >= c->x + c->w || p->x + p->w <= c->x || p->y >= c->y + c->h ||
            p->y + p->h <= c->y) {
            continue;
        }
        if (c->hp >= c->hp_max) {
            continue;
        }
        c->hp = PR_MIN(c->hp + PICKUP_HEALTH_HP, c->hp_max);
        p->taken = 1;
        world_sound(w, SOUND_PICKUP, c->x + c->w / 2, c->y + c->h / 2, 0.8f);
        n++;
    }
    return n;
}
