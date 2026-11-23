#include "sim/scene.h"

#include "core/rng.h"
#include "sim/creature.h"
#include "sim/edit.h"

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
