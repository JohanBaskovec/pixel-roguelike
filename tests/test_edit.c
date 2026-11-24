#include "test.h"

#include "sim/edit.h"
#include "sim/sim.h"

#include <math.h>

void test_edit_single_pixel(void);
void test_edit_circle_area(void);
void test_edit_stroke_has_no_gaps(void);
void test_edit_only_void(void);
void test_edit_erase(void);
void test_gas_painted_at_pressure_up_to_liquid(void);

void test_edit_single_pixel(void) {
    World *w = test_world_box(64, 64, 1);
    Edit e = {10, 20, 10, 20, 0, EDIT_CIRCLE, ELEM_IRON, 0, EDIT_PAINT, 0.0f};
    EXPECT_EQ(sim_apply_edit(w, &e), 1);
    EXPECT(test_cell_is(w, 10, 20, ELEM_IRON));
    e.shape = EDIT_SQUARE;
    e.x0 = e.x1 = 30;
    EXPECT_EQ(sim_apply_edit(w, &e), 1);
    world_destroy(w);
}

void test_edit_circle_area(void) {
    World *w = test_world_box(128, 128, 1);
    Edit e = {64, 64, 64, 64, 20, EDIT_CIRCLE, ELEM_ROCK, 0, EDIT_PAINT, 0.0f};
    int64_t n = sim_apply_edit(w, &e);
    // pi * 20.5^2 = 1320
    EXPECT(n > 1250 && n < 1400);
    e.shape = EDIT_SQUARE;
    e.x0 = e.x1 = e.y0 = e.y1 = 30;
    e.radius = 5;
    EXPECT_EQ(sim_apply_edit(w, &e), 121);
    world_destroy(w);
}

void test_edit_stroke_has_no_gaps(void) {
    World *w = test_world_box(128, 128, 1);
    Edit e = {5, 5, 120, 90, 0, EDIT_CIRCLE, ELEM_IRON, 0, EDIT_PAINT, 0.0f};
    sim_apply_edit(w, &e);
    // Every column between the endpoints contains part of the line (dx > dy).
    for (int x = 5; x <= 120; x++) {
        int found = 0;
        for (int y = 0; y < 128; y++) {
            found += test_cell_is(w, x, y, ELEM_IRON);
        }
        EXPECT(found >= 1);
    }
    world_destroy(w);
}

void test_edit_only_void(void) {
    World *w = test_world_box(64, 64, 1);
    // A brush over the corner only fills the interior void cells; the dirt walls stay.
    Edit wall = {0, 0, 0, 0, 3, EDIT_CIRCLE, ELEM_WATER, 1, EDIT_PAINT, 0.0f};
    EXPECT(sim_apply_edit(w, &wall) > 0);
    EXPECT(test_cell_is(w, 0, 0, ELEM_DIRT));
    EXPECT(test_cell_is(w, 1, 1, ELEM_WATER));
    EXPECT_EQ(test_count(w, ELEM_DIRT), 64 * 4 - 4);
    world_destroy(w);
}

void test_edit_erase(void) {
    World *w = test_world_box(64, 64, 1);
    Edit e = {0, 32, 0, 32, 4, EDIT_CIRCLE, ELEM_VOID, 0, EDIT_PAINT, 0.0f};
    sim_apply_edit(w, &e);
    EXPECT(test_cell_is(w, 0, 32, ELEM_VOID));
    world_destroy(w);
}

// A single cell of void shut in iron, in a world of its own.
static World *sealed_cell(uint64_t seed) {
    World *w = test_world_box(64, 64, seed);
    for (int y = 20; y <= 24; y++) {
        Edit iron = {20, y, 24, y, 0, EDIT_SQUARE, ELEM_IRON, 0, EDIT_PAINT, 0.0f};
        sim_apply_edit(w, &iron);
    }
    Edit hole = {22, 22, 22, 22, 0, EDIT_SQUARE, ELEM_VOID, 0, EDIT_PAINT, 0.0f};
    sim_apply_edit(w, &hole);
    return w;
}

void test_gas_painted_at_pressure_up_to_liquid(void) {
    // The gas brush paints at the pressure asked for (the engine has no cap: a sealed cell holds
    // it), up to the density of the gas's liquid (steam: its saturation pressure, above which it
    // would condense).
    World *w = sealed_cell(160);
    Edit g = {22, 22, 22, 22, 0, EDIT_SQUARE, ELEM_HYDROGEN, 0, EDIT_PAINT, 100.0f};
    sim_apply_edit(w, &g);
    float p = world_gas_pressure(w, 22, 22);
    EXPECT(fabsf(p / (100.0f * P_ATM) - 1.0f) < 0.01f);
    sim_run_ticks(w, 60);
    EXPECT(fabsf(world_gas_pressure(w, 22, 22) / p - 1.0f) < 0.01f); // sealed: it stays
    EXPECT(w->cells[world_index(w, 22, 22)].element == ELEM_GAS);
    // Far too much: limited to the density of liquid hydrogen (70.8 kg/m^3).
    g.pressure = 1e7f;
    sim_apply_edit(w, &g);
    float m = gas_total(&w->gas[world_index(w, 22, 22)]);
    EXPECT(fabsf(m / (70.8f * CELL_VOLUME_M3) - 1.0f) < 0.01f);
    world_destroy(w);
    // Steam: no more than it can hold at its temperature, so it does not condense.
    w = sealed_cell(161);
    Edit s = {22, 22, 22, 22, 0, EDIT_SQUARE, ELEM_STEAM, 0, EDIT_PAINT, 100.0f};
    sim_apply_edit(w, &s);
    float t = w->temp[world_index(w, 22, 22)];
    EXPECT(world_gas_pressure(w, 22, 22) <= water_saturation_pressure(t) * 1.01f);
    sim_run_ticks(w, 20);
    EXPECT(w->cells[world_index(w, 22, 22)].element == ELEM_GAS);
    world_destroy(w);
}
