#include "test.h"

#include "sim/edit.h"
#include "sim/sim.h"

#include <string.h>

void test_solids_never_move(void);
void test_water_falls_and_levels(void);
void test_water_sinks_through_gas(void);
void test_gas_expands_into_void(void);
void test_co2_settles_below_oxygen(void);
void test_hydrogen_rises_above_oxygen(void);
void test_counts_conserved(void);
void test_deterministic(void);
void test_settled_world_sleeps(void);
void test_no_diagonal_leak(void);

void test_solids_never_move(void) {
    World *w = test_world_box(64, 64, 1);
    Edit e = {20, 10, 40, 10, 2, EDIT_CIRCLE, ELEM_IRON, 0};
    sim_apply_edit(w, &e);
    e.element = ELEM_ICE;
    e.y0 = e.y1 = 30;
    sim_apply_edit(w, &e);
    Cell before[64 * 64];
    memcpy(before, w->cells, sizeof(before));
    sim_run_ticks(w, 200);
    for (int i = 0; i < 64 * 64; i++) {
        if (g_elem_behavior[before[i].element] == BEHAVIOR_STATIC) {
            EXPECT_EQ(w->cells[i].element, before[i].element);
        }
    }
    world_destroy(w);
}

void test_water_falls_and_levels(void) {
    World *w = test_world_box(64, 64, 2);
    Edit e = {32, 12, 32, 12, 8, EDIT_CIRCLE, ELEM_WATER, 0};
    int64_t n = sim_apply_edit(w, &e);
    sim_run_ticks(w, 1500);
    EXPECT_EQ(test_count(w, ELEM_WATER), n);
    // No water cell floats above void.
    int floating = 0;
    for (int y = 0; y < 63; y++) {
        for (int x = 0; x < 64; x++) {
            if (test_cell_is(w, x, y, ELEM_WATER) && test_cell_is(w, x, y + 1, ELEM_VOID)) {
                floating++;
            }
        }
    }
    EXPECT_EQ(floating, 0);
    // The surface is flat: column heights differ by at most one cell.
    int min_h = 1000, max_h = -1;
    for (int x = 1; x < 63; x++) {
        int h = 0;
        for (int y = 1; y < 63; y++) {
            h += test_cell_is(w, x, y, ELEM_WATER);
        }
        min_h = PR_MIN(min_h, h);
        max_h = PR_MAX(max_h, h);
    }
    EXPECT(max_h - min_h <= 1);
    world_destroy(w);
}

void test_water_sinks_through_gas(void) {
    World *w = test_world_box(64, 64, 3);
    Edit fill = {32, 32, 32, 32, 40, EDIT_SQUARE, ELEM_OXYGEN, 1};
    sim_apply_edit(w, &fill);
    Edit water = {32, 8, 32, 8, 5, EDIT_CIRCLE, ELEM_WATER, 0};
    sim_apply_edit(w, &water);
    sim_run_ticks(w, 600);
    EXPECT(test_mean_y(w, ELEM_WATER) > 58.0);
    world_destroy(w);
}

void test_gas_expands_into_void(void) {
    World *w = test_world_box(64, 64, 4);
    Edit e = {32, 56, 32, 56, 4, EDIT_CIRCLE, ELEM_OXYGEN, 0};
    sim_apply_edit(w, &e);
    sim_run_ticks(w, 1500);
    int top_half = 0;
    for (int y = 1; y < 32; y++) {
        for (int x = 1; x < 63; x++) {
            top_half += test_cell_is(w, x, y, ELEM_OXYGEN);
        }
    }
    EXPECT(top_half > 0);
    world_destroy(w);
}

// Box completely filled with gas, heavy layer on top of a light one; they should invert.
static World *gas_layers(ElementId top, ElementId bottom, uint64_t seed) {
    World *w = test_world_box(64, 64, seed);
    Edit a = {1, 1, 62, 31, 0, EDIT_SQUARE, top, 0};
    Edit b = {1, 32, 62, 62, 0, EDIT_SQUARE, bottom, 0};
    for (int y = 1; y <= 31; y++) {
        a.y0 = a.y1 = y;
        sim_apply_edit(w, &a);
    }
    for (int y = 32; y <= 62; y++) {
        b.y0 = b.y1 = y;
        sim_apply_edit(w, &b);
    }
    return w;
}

void test_co2_settles_below_oxygen(void) {
    World *w = gas_layers(ELEM_CO2, ELEM_OXYGEN, 5);
    EXPECT(test_mean_y(w, ELEM_CO2) < test_mean_y(w, ELEM_OXYGEN));
    sim_run_ticks(w, 3000);
    EXPECT(test_mean_y(w, ELEM_CO2) > test_mean_y(w, ELEM_OXYGEN) + 10.0);
    world_destroy(w);
}

void test_hydrogen_rises_above_oxygen(void) {
    World *w = gas_layers(ELEM_OXYGEN, ELEM_HYDROGEN, 6);
    sim_run_ticks(w, 2000);
    EXPECT(test_mean_y(w, ELEM_HYDROGEN) + 10.0 < test_mean_y(w, ELEM_OXYGEN));
    world_destroy(w);
}

void test_counts_conserved(void) {
    World *w = test_world_box(128, 128, 7);
    ElementId els[] = {ELEM_WATER, ELEM_OXYGEN, ELEM_CO2, ELEM_HYDROGEN, ELEM_ROCK};
    for (int i = 0; i < 5; i++) {
        Edit e = {20 + i * 20, 30, 30 + i * 18, 90, 6, EDIT_CIRCLE, els[i], 0};
        sim_apply_edit(w, &e);
    }
    int64_t before[ELEM_COUNT];
    for (int e = 0; e < ELEM_COUNT; e++) {
        before[e] = test_count(w, (ElementId)e);
    }
    sim_run_ticks(w, 1000);
    for (int e = 0; e < ELEM_COUNT; e++) {
        EXPECT_EQ(test_count(w, (ElementId)e), before[e]);
    }
    world_destroy(w);
}

static World *scripted_world(uint64_t seed) {
    World *w = test_world_box(128, 128, seed);
    for (int t = 0; t < 300; t++) {
        if (t % 30 == 0) {
            ElementId e = (ElementId)(ELEM_WATER + (t / 30) % 4);
            Edit ed = {20 + t / 4, 20, 40 + t / 5, 30, 5, EDIT_CIRCLE, e, 0};
            sim_apply_edit(w, &ed);
        }
        sim_tick(w, NULL);
    }
    return w;
}

void test_deterministic(void) {
    World *a = scripted_world(42);
    World *b = scripted_world(42);
    size_t n = (size_t)a->width * a->height;
    EXPECT(memcmp(a->cells, b->cells, n * sizeof(Cell)) == 0);
    EXPECT(memcmp(a->temp, b->temp, n * sizeof(float)) == 0);
    World *c = scripted_world(43);
    EXPECT(memcmp(a->cells, c->cells, n * sizeof(Cell)) != 0);
    world_destroy(a);
    world_destroy(b);
    world_destroy(c);
}

void test_settled_world_sleeps(void) {
    // A pool of complete rows has nowhere to go: everything sleeps.
    World *w = test_world_box(128, 128, 8);
    Edit row = {1, 0, 126, 0, 0, EDIT_SQUARE, ELEM_WATER, 0};
    for (int y = 100; y <= 126; y++) {
        row.y0 = row.y1 = y;
        sim_apply_edit(w, &row);
    }
    sim_run_ticks(w, 10);
    SimStats s;
    sim_tick(w, &s);
    EXPECT_EQ(s.cells_visited, 0);
    EXPECT_EQ(s.chunks_active, 0);
    world_destroy(w);

    // A poured pool (~16 rows deep) settles; only the rows at its surface may stay awake
    // (shimmer of the partial top row, plus the neighbours it touches). The interior sleeps.
    w = test_world_box(128, 128, 8);
    Edit e = {64, 40, 64, 40, 25, EDIT_CIRCLE, ELEM_WATER, 0};
    int64_t n = sim_apply_edit(w, &e);
    sim_run_ticks(w, 3000);
    sim_tick(w, &s);
    EXPECT(s.cells_visited <= 3 * 126);
    EXPECT(s.cells_visited < (uint64_t)n / 4);
    world_destroy(w);
}

void test_no_diagonal_leak(void) {
    // Water sits on an 8-connected diagonal of iron. It must not slip through the corners.
    World *w = test_world_from_ascii("####################\n"
                                     "#~~~~~~~~~~~~~~~~~~#\n"
                                     "#~~~~~~~~~~~~~~~~~~#\n"
                                     "#I~~~~~~~~~~~~~~~~I#\n"
                                     "# I~~~~~~~~~~~~~~I #\n"
                                     "#  I~~~~~~~~~~~~I  #\n"
                                     "#   I~~~~~~~~~~I   #\n"
                                     "#    IIIIIIIIII    #\n"
                                     "#                  #\n"
                                     "####################\n",
                                     9);
    sim_run_ticks(w, 300);
    int below = 0;
    for (int x = 1; x < 19; x++) {
        below += test_cell_is(w, x, 8, ELEM_WATER);
    }
    EXPECT_EQ(below, 0);
    world_destroy(w);
}
