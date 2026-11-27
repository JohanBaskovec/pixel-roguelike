#include "test.h"

#include "sim/edit.h"
#include "sim/sim.h"

#include <math.h>

void test_hot_block_radiates_to_the_walls(void);
void test_cool_matter_does_not_radiate(void);
void test_lava_film_crusts_over(void);

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT, 0.0f, 0.0f};
        sim_apply_edit(w, &ed);
    }
}

static void box(World *w) {
    fill(w, 0, 0, w->width - 1, 0, ELEM_ROCK);
    fill(w, 0, w->height - 1, w->width - 1, w->height - 1, ELEM_ROCK);
    fill(w, 0, 0, 0, w->height - 1, ELEM_ROCK);
    fill(w, w->width - 1, 0, w->width - 1, w->height - 1, ELEM_ROCK);
}

static void heat_block(World *w, int x0, int y0, int x1, int y1, float k) {
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            w->temp[world_index(w, x, y)] = k;
        }
    }
    world_wake_heat(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
}

static double stored_energy(const World *w) {
    double sum = 0.0;
    for (size_t i = 0; i < (size_t)w->width * (size_t)w->height; i++) {
        sum += (double)g_elem_cap[w->cells[i].element] * (double)w->temp[i];
    }
    return sum;
}

// An iron block at 1000 K in the middle of a rock box in vacuum: nothing conducts (void between),
// yet it cools and the walls warm, and almost all the heat arrives in the walls (a ray that finds
// nothing within RAD_RANGE leaves; the corners are a little further than that).
void test_hot_block_radiates_to_the_walls(void) {
    World *w = test_world_box(64, 64, 11);
    box(w); // rock walls 1 cell thick around 62 x 62 of void
    fill(w, 31, 31, 32, 32, ELEM_IRON);
    heat_block(w, 31, 31, 32, 32, 1000.0f);
    double before = stored_energy(w);
    sim_run_ticks(w, 300);
    double after = stored_energy(w);
    EXPECT(w->temp[world_index(w, 31, 31)] < 950.0f); // cooled (a 2 x 2 block: 1770 J/K)
    float hottest_wall = 0.0f;
    for (int k = 1; k < 63; k++) {
        hottest_wall = fmaxf(hottest_wall, w->temp[world_index(w, k, 0)]);
        hottest_wall = fmaxf(hottest_wall, w->temp[world_index(w, 0, k)]);
    }
    EXPECT(hottest_wall > T_ROOM + 2.0f); // the walls took heat
    EXPECT(after < before);               // some left for good (rays beyond the range)
    EXPECT(after > 0.8 * before);         // but most of it is in the walls
    world_destroy(w);
}

void test_cool_matter_does_not_radiate(void) {
    World *w = test_world_box(64, 64, 12);
    box(w);
    fill(w, 31, 31, 32, 32, ELEM_IRON);
    heat_block(w, 31, 31, 32, 32, RAD_MIN_K - 50.0f);
    sim_run_ticks(w, 120);
    EXPECT(w->temp[world_index(w, 31, 31)] == RAD_MIN_K - 50.0f); // in vacuum: nothing else to lose
    EXPECT(w->temp[world_index(w, 0, 31)] == T_ROOM);
    world_destroy(w);
}

// A film of lava (3 cells) on a floor, in air: its top cools through radiation and freezes into
// rock within seconds, and heat from below keeps the rest molten a little longer; after 15 s it
// is all rock.
void test_lava_film_crusts_over(void) {
    World *w = test_world_box(128, 64, 13);
    fill(w, 1, 1, 126, 62, EDIT_AIR);
    fill(w, 20, 60, 100, 62, ELEM_LAVA);
    int64_t lava0 = test_count(w, ELEM_LAVA);
    sim_run_ticks(w, 300); // 5 s: a few cells have frozen
    EXPECT(test_count(w, ELEM_LAVA) > lava0 * 8 / 10);
    sim_run_ticks(w, 300); // 10 s: most of it is rock
    EXPECT(test_count(w, ELEM_LAVA) < lava0 * 4 / 10);
    EXPECT(test_count(w, ELEM_ROCK) > lava0 / 2);
    sim_run_ticks(w, 600); // 20 s: all of it
    EXPECT(test_count(w, ELEM_LAVA) < lava0 / 50);
    world_destroy(w);
}
