#include "sim/scene.h"

#include "core/rng.h"
#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/enemy.h"
#include "sim/particle.h"
#include "sim/pickup.h"

#include <math.h>

static void fill_rect(World *w, int x0, int y0, int x1, int y1, ElementId e, Rng *rng) {
    x0 = PR_MAX(x0, 0);
    y0 = PR_MAX(y0, 0);
    x1 = PR_MIN(x1, w->width - 1);
    y1 = PR_MIN(y1, w->height - 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            world_set_cell(w, x, y, e, rng_next(rng));
        }
    }
    world_wake_rect(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
    world_wake_heat(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
}

void scene_empty_box(World *w, int thickness) {
    Rng rng = rng_from_seed(w->seed ^ 0xB0C5ull);
    world_clear(w);
    int t = thickness - 1;
    int W = w->width - 1, H = w->height - 1;
    fill_rect(w, 0, 0, W, t, ELEM_DIRT, &rng);     // ceiling
    fill_rect(w, 0, H - t, W, H, ELEM_DIRT, &rng); // floor
    fill_rect(w, 0, 0, t, H, ELEM_DIRT, &rng);     // left wall
    fill_rect(w, W - t, 0, W, H, ELEM_DIRT, &rng); // right wall
}

static void blob(World *w, int x, int y, int r, ElementId e) {
    Edit ed = {x, y, x, y, r, EDIT_CIRCLE, e, 0, EDIT_PAINT};
    sim_apply_edit(w, &ed);
}

void scene_demo(World *w) {
    scene_empty_box(w, SCENE_WALL_THICKNESS);
    int W = w->width, H = w->height;
    Rng rng = rng_from_seed(w->seed ^ 0xDE30ull);
    // A rock ledge.
    fill_rect(w, W / 8, H / 2, W / 2, H / 2 + 12, ELEM_ROCK, &rng);
    // An iron U-tube standing on the floor: two arms joined by a channel at the bottom, water high
    // in the left arm and low in the right. Water pressure brings them level.
    int x0 = W * 5 / 8, x1 = x0 + W / 6, xm = (x0 + x1) / 2;
    int y0 = H / 2, y1 = H - 17;
    fill_rect(w, x0, y0, x1, y1, ELEM_IRON, &rng);
    fill_rect(w, x0 + 8, y0, xm - 6, y1 - 8, ELEM_VOID, &rng);      // left arm
    fill_rect(w, xm + 6, y0, x1 - 8, y1 - 8, ELEM_VOID, &rng);      // right arm
    fill_rect(w, x0 + 8, y1 - 48, x1 - 8, y1 - 8, ELEM_VOID, &rng); // channel
    fill_rect(w, x0 + 8, y0 + (y1 - y0) / 6, xm - 6, y1 - 8, ELEM_WATER, &rng);
    fill_rect(w, xm - 6, y1 - 48, x1 - 8, y1 - 8, ELEM_WATER, &rng);
    // A tall rock tank on the floor with a hole near its foot: the water leaves as a jet (M4b),
    // fast at first, slowing as the tank empties.
    int tx0 = W / 32, tx1 = tx0 + W / 16;
    int ty0 = H / 2 + 60, ty1 = H - 17;
    fill_rect(w, tx0, ty0, tx1, ty1, ELEM_ROCK, &rng);
    fill_rect(w, tx0 + 6, ty0 - 1, tx1 - 6, ty1 - 6, ELEM_WATER, &rng);
    fill_rect(w, tx1 - 5, ty1 - 30, tx1, ty1 - 28, ELEM_VOID, &rng); // the hole
    // A room of air on the floor, right of the U-tube, with a creature wandering in it (M4c).
    int rx0 = W * 13 / 16, rx1 = W - 40, ry0 = H - 17 - 160, ry1 = H - 17;
    fill_rect(w, rx0, ry0, rx1, ry1, ELEM_ROCK, &rng);
    for (int y = ry0 + 6; y <= ry1 - 6; y++) {
        Edit air = {rx0 + 6, y, rx1 - 6, y, 0, EDIT_SQUARE, EDIT_AIR, 0, EDIT_PAINT};
        sim_apply_edit(w, &air);
    }
    int id = creature_spawn(w, (rx0 + rx1) / 2, ry1 - 6 - CREATURE_H);
    if (id >= 0) {
        w->creatures[id].wander = 1;
    }
    // Water on the ledge, gases at mixed heights.
    blob(w, W / 4, H / 2 - 60, 50, ELEM_WATER);
    blob(w, W * 3 / 4, H / 4, 60, ELEM_CO2);
    blob(w, W * 3 / 4, H / 2, 60, ELEM_OXYGEN);
    blob(w, W / 2, H * 3 / 4, 50, ELEM_HYDROGEN);
    blob(w, W / 8, H / 6, 30, ELEM_ICE);
}

typedef struct Maze {
    int cols, rows; // rooms
    int px, py;     // pitch of a room: its size plus a wall
    int x0, y0;     // top-left of room (0, 0)
} Maze;

static Maze maze_layout(const World *w) {
    int t = SCENE_WALL_THICKNESS;
    Maze m;
    m.px = MAZE_ROOM_W + MAZE_WALL;
    m.py = MAZE_ROOM_H + MAZE_WALL;
    m.cols = PR_MAX((w->width - 2 * t + MAZE_WALL) / m.px, 1);
    m.rows = PR_MAX((w->height - 2 * t + MAZE_WALL) / m.py, 1);
    m.x0 = t;
    m.y0 = t;
    return m;
}

void scene_level_start(const World *w, int *x, int *y) {
    Maze m = maze_layout(w);
    *x = m.x0 + (m.cols / 2) * m.px + MAZE_ROOM_W / 2;
    *y = m.y0 + (m.rows / 2) * m.py + MAZE_ROOM_H / 2;
}

enum { LINK_RIGHT = 1, LINK_DOWN = 2 };

void scene_level(World *w) {
    scene_empty_box(w, SCENE_WALL_THICKNESS);
    Rng rng = rng_from_seed(w->seed ^ 0x1E7E1ull);
    int t = SCENE_WALL_THICKNESS, W = w->width, H = w->height;
    // Dirt everywhere, with boulders of rock in it.
    fill_rect(w, t, t, W - t - 1, H - t - 1, ELEM_DIRT, &rng);
    for (int k = 0; k < W / 64; k++) {
        int bx = t + (int)rng_range(&rng, (uint32_t)(W - 2 * t));
        int by = t + (int)rng_range(&rng, (uint32_t)(H - 2 * t));
        int r = 6 + (int)rng_range(&rng, 22);
        for (int y = by - r; y <= by + r; y++) {
            for (int x = bx - r; x <= bx + r; x++) {
                if ((x - bx) * (x - bx) + (y - by) * (y - by) <= r * r &&
                    world_elem(w, x, y) == ELEM_DIRT) {
                    world_set_cell(w, x, y, ELEM_ROCK, rng_next(&rng));
                }
            }
        }
    }
    // A perfect maze over the rooms (depth-first from room (0, 0)): links[] says which neighbours
    // each room opens to (to the right, and downwards).
    Maze m = maze_layout(w);
    int n = m.cols * m.rows;
    uint8_t links[4096] = {0};
    uint8_t seen[4096] = {0};
    int stack[4096], top = 0;
    PR_ASSERT(n <= 4096);
    (void)n;
    stack[top++] = 0;
    seen[0] = 1;
    while (top > 0) {
        int cur = stack[top - 1], cx = cur % m.cols, cy = cur / m.cols;
        int nb[4], dir[4], cnt = 0; // dir: 0 right, 1 down, 2 left, 3 up
        if (cx + 1 < m.cols && !seen[cur + 1]) {
            nb[cnt] = cur + 1, dir[cnt++] = 0;
        }
        if (cy + 1 < m.rows && !seen[cur + m.cols]) {
            nb[cnt] = cur + m.cols, dir[cnt++] = 1;
        }
        if (cx > 0 && !seen[cur - 1]) {
            nb[cnt] = cur - 1, dir[cnt++] = 2;
        }
        if (cy > 0 && !seen[cur - m.cols]) {
            nb[cnt] = cur - m.cols, dir[cnt++] = 3;
        }
        if (cnt == 0) {
            top--;
            continue;
        }
        int k = (int)rng_range(&rng, (uint32_t)cnt);
        switch (dir[k]) {
        case 0: links[cur] |= LINK_RIGHT; break;
        case 1: links[cur] |= LINK_DOWN; break;
        case 2: links[nb[k]] |= LINK_RIGHT; break;
        default: links[nb[k]] |= LINK_DOWN; break;
        }
        seen[nb[k]] = 1;
        stack[top++] = nb[k];
    }
    // Carve the rooms and the openings.
    for (int cy = 0; cy < m.rows; cy++) {
        for (int cx = 0; cx < m.cols; cx++) {
            int x0 = m.x0 + cx * m.px, y0 = m.y0 + cy * m.py;
            fill_rect(w, x0, y0, x0 + MAZE_ROOM_W - 1, y0 + MAZE_ROOM_H - 1, ELEM_VOID, &rng);
            if (links[cy * m.cols + cx] & LINK_RIGHT) { // a doorway as tall as the room
                fill_rect(w, x0 + MAZE_ROOM_W, y0, x0 + m.px - 1, y0 + MAZE_ROOM_H - 1, ELEM_VOID,
                          &rng);
            }
            if (links[cy * m.cols + cx] & LINK_DOWN) { // a shaft as wide as the room is tall
                int sx = x0 + (int)rng_range(&rng, (uint32_t)(MAZE_ROOM_W - MAZE_ROOM_H + 1));
                fill_rect(w, sx, y0 + MAZE_ROOM_H, sx + MAZE_ROOM_H - 1, y0 + m.py - 1, ELEM_VOID,
                          &rng);
            }
        }
    }
    // Air in every open cell.
    for (int y = t; y < H - t; y++) {
        Edit air = {t, y, W - t - 1, y, 0, EDIT_SQUARE, EDIT_AIR, 1, EDIT_PAINT};
        sim_apply_edit(w, &air);
    }
    // Crates and monsters scattered over the rooms (not the start room, which only gets a crate of
    // ammunition near the player).
    int sx, sy;
    scene_level_start(w, &sx, &sy);
    for (int cy = 0; cy < m.rows; cy++) {
        for (int cx = 0; cx < m.cols; cx++) {
            int x0 = m.x0 + cx * m.px, y0 = m.y0 + cy * m.py, floor_y = y0 + MAZE_ROOM_H - 1;
            bool start = sx >= x0 && sx < x0 + MAZE_ROOM_W && sy >= y0 && sy < y0 + MAZE_ROOM_H;
            if (start) {
                pickup_add(w, PICKUP_AMMO, sx + 60, floor_y);
                continue;
            }
            uint32_t r = rng_range(&rng, 100);
            int kind = r < 10 ? 0 : r < 18 ? 1 : r < 30 ? 2 : r < 38 ? 3 : -1;
            if (kind < 0) {
                continue;
            }
            // A spot on the floor (on solid ground, not over a shaft), or anywhere for a flyer.
            for (int tries = 0; tries < 8; tries++) {
                int x = x0 + 8 + (int)rng_range(&rng, (uint32_t)(MAZE_ROOM_W - 16 - FLYER_W));
                bool ground = true;
                for (int k = 0; k < FLYER_W; k++) {
                    ground = ground && !particle_passes(w, x + k, floor_y + 1);
                }
                if (kind == 1) { // flyer, in the air
                    int y = y0 + 6 + (int)rng_range(&rng, (uint32_t)(MAZE_ROOM_H - 12 - FLYER_H));
                    if (enemy_spawn(w, KIND_FLYER, x, y) >= 0) {
                        break;
                    }
                } else if (ground) {
                    if (kind == 0) {
                        if (enemy_spawn(w, KIND_WALKER, x, floor_y - WALKER_H + 1) >= 0) {
                            break;
                        }
                    } else {
                        pickup_add(w, kind == 2 ? PICKUP_AMMO : PICKUP_HEALTH, x + PICKUP_W / 2,
                                   floor_y);
                        break;
                    }
                }
            }
        }
    }
    world_wake_rect(w, 0, 0, w->width - 1, w->height - 1);
    world_wake_heat(w, 0, 0, w->width - 1, w->height - 1);
}

int scene_spawn_player(World *w, int x) {
    return scene_spawn_player_from(w, x, SCENE_WALL_THICKNESS);
}

int scene_spawn_player_from(World *w, int x, int y) {
    for (ElementId e = world_elem(w, x, y);
         y < w->height && (e == ELEM_VOID || g_elem_behavior[e] == BEHAVIOR_GAS);
         e = world_elem(w, x, ++y)) {
    }
    for (int k = 0; k < 64; k++, y--) { // up from the ground until the body fits
        int id = creature_spawn_sized(w, x - PLAYER_W / 2, y - PLAYER_H, PLAYER_W, PLAYER_H);
        if (id >= 0) {
            Creature *c = &w->creatures[id];
            c->speed = PLAYER_SPEED;
            c->kind = KIND_PLAYER;
            c->aimed = 1;
            return id;
        }
    }
    return -1;
}

void scene_bench(World *w, float fill) {
    scene_empty_box(w, SCENE_WALL_THICKNESS);
    Rng rng = rng_from_seed(w->seed ^ 0xBE4C11ull);
    int t = SCENE_WALL_THICKNESS;
    int floor_y = w->height - w->height / 4;
    int band_top = floor_y - (int)((float)(floor_y - t) * PR_CLAMP(fill, 0.0f, 1.0f));
    for (int y = t; y < w->height - t; y++) {
        for (int x = t; x < w->width - t; x++) {
            uint32_t r = rng_range(&rng, 100);
            ElementId e;
            if (y >= floor_y) {
                e = (r < 85) ? ELEM_ROCK : ELEM_DIRT;
            } else if (y < band_top) {
                e = ELEM_VOID;
            } else if (r < 25) {
                e = ELEM_WATER;
            } else if (r < 40) {
                e = ELEM_OXYGEN;
            } else if (r < 55) {
                e = ELEM_CO2;
            } else if (r < 65) {
                e = ELEM_HYDROGEN;
            } else {
                e = ELEM_VOID;
            }
            world_set_cell(w, x, y, e, rng_next(&rng));
        }
    }
    world_wake_rect(w, 0, 0, w->width - 1, w->height - 1);
    world_wake_heat(w, 0, 0, w->width - 1, w->height - 1);
}
