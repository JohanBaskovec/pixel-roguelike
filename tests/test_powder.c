#include "test.h"

#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/sim.h"

#include <stdlib.h>

void test_sand_piles_up(void);
void test_sand_sinks_in_water(void);
void test_digging_makes_rubble(void);
void test_creature_stands_on_sand(void);
void test_rock_and_iron_melt_and_freeze(void);
void test_lava_boils_water(void);
void test_oil_floats_on_water(void);
void test_oil_manometer(void);

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT, 0.0f, 0.0f};
        sim_apply_edit(w, &ed);
    }
}

// Height of the column at x: cells from the floor (y = 126) up to the first non-e cell.
static int column_height(const World *w, int x, ElementId e) {
    int h = 0;
    while (h < 126 && test_cell_is(w, x, 126 - h, e)) {
        h++;
    }
    return h;
}

void test_sand_piles_up(void) {
    // A block of sand dropped on the floor slumps into a heap: no column more than one cell
    // higher than its neighbour (45 degrees or less), and every grain is still there.
    World *w = test_world_box(128, 128, 81);
    fill(w, 54, 20, 73, 59, ELEM_SAND); // 20 x 40
    int64_t n = test_count(w, ELEM_SAND);
    sim_run_ticks(w, 1200);
    EXPECT_EQ(test_count(w, ELEM_SAND), n);
    int steepest = 0, peak = 0;
    for (int x = 2; x < 126; x++) {
        int a = column_height(w, x, ELEM_SAND), b = column_height(w, x + 1, ELEM_SAND);
        steepest = PR_MAX(steepest, a > b ? a - b : b - a);
        peak = PR_MAX(peak, a);
    }
    EXPECT(steepest <= 1);
    EXPECT(peak < 40 && peak > 15); // a heap, not a tower nor a flat layer
    // Settled: asleep.
    SimStats st;
    sim_tick(w, &st);
    EXPECT(st.cells_visited < 50);
    world_destroy(w);
}

void test_sand_sinks_in_water(void) {
    // Sand poured on a pool sinks to the bottom; the water ends up above it.
    World *w = test_world_box(64, 128, 82);
    fill(w, 1, 80, 62, 126, ELEM_WATER);
    fill(w, 20, 40, 43, 49, ELEM_SAND);
    int64_t water = test_count(w, ELEM_WATER), sand = test_count(w, ELEM_SAND);
    sim_run_ticks(w, 2400);
    EXPECT_EQ(test_count(w, ELEM_WATER), water);
    EXPECT_EQ(test_count(w, ELEM_SAND), sand);
    EXPECT(test_mean_y(w, ELEM_SAND) > test_mean_y(w, ELEM_WATER) + 10.0);
    world_destroy(w);
}

void test_digging_makes_rubble(void) {
    // Digging through a rock shelf turns the rock into rubble, which falls out to the floor; dug
    // dirt becomes loose soil, which falls too.
    World *w = test_world_box(128, 128, 83);
    fill(w, 20, 40, 80, 60, ELEM_ROCK);
    fill(w, 95, 70, 120, 75, ELEM_DIRT);
    int64_t rock = test_count(w, ELEM_ROCK);
    Edit dig = {50, 55, 50, 55, 6, EDIT_CIRCLE, ELEM_VOID, 0, EDIT_DIG, 0.0f, 0.0f}; // reaches the underside
    sim_apply_edit(w, &dig);
    Edit dig2 = {107, 72, 107, 72, 3, EDIT_SQUARE, ELEM_VOID, 0, EDIT_DIG, 0.0f, 0.0f}; // through the dirt
    sim_apply_edit(w, &dig2);
    int64_t rubble = test_count(w, ELEM_RUBBLE);
    EXPECT(rubble > 0);
    EXPECT_EQ(test_count(w, ELEM_ROCK) + rubble, rock);
    EXPECT(test_count(w, ELEM_SOIL) > 0);
    sim_run_ticks(w, 900);
    EXPECT(test_mean_y(w, ELEM_SOIL) > 115.0);
    EXPECT(test_mean_y(w, ELEM_RUBBLE) > 110.0);
    EXPECT_EQ(test_count(w, ELEM_RUBBLE), rubble);
    world_destroy(w);
}

void test_creature_stands_on_sand(void) {
    World *w = test_world_box(128, 128, 84);
    fill(w, 1, 110, 126, 126, ELEM_SAND);
    sim_run_ticks(w, 60);
    int id = creature_spawn(w, 60, 30);
    sim_run_ticks(w, 300);
    EXPECT_EQ(w->creatures[id].y + CREATURE_H - 1, 109); // on top of the sand
    EXPECT(w->creatures[id].grounded);
    world_destroy(w);
}

// ---- Molten matter and more liquids (M5) ----

void test_rock_and_iron_melt_and_freeze(void) {
    // Single cells held past their melting point by more than the latent heat change phase:
    // rock -> lava (1500 K, 400 kJ/kg), iron -> molten iron (1811 K, 247 kJ/kg), and back.
    World *w = test_world_box(64, 64, 85);
    fill(w, 10, 62, 10, 62, ELEM_ROCK);
    fill(w, 20, 62, 20, 62, ELEM_IRON);
    fill(w, 30, 62, 30, 62, ELEM_LAVA);
    fill(w, 40, 62, 40, 62, ELEM_MOLTEN_IRON);
    w->temp[world_index(w, 10, 62)] = 1500.0f + 400e3f / 790.0f + 5.0f;
    w->temp[world_index(w, 20, 62)] = 1811.0f + 247e3f / 449.0f + 5.0f;
    w->temp[world_index(w, 30, 62)] = 1500.0f - 400e3f / 1100.0f - 5.0f;
    w->temp[world_index(w, 40, 62)] = 1811.0f - 247e3f / 820.0f - 5.0f;
    world_wake_heat(w, 0, 0, 63, 63);
    world_wake_rect(w, 0, 0, 63, 63);
    sim_tick(w, NULL);
    EXPECT_EQ(test_count(w, ELEM_LAVA), 1);        // from the rock
    EXPECT_EQ(test_count(w, ELEM_MOLTEN_IRON), 1); // from the iron
    EXPECT_EQ(test_count(w, ELEM_ROCK), 1);        // from the lava
    EXPECT_EQ(test_count(w, ELEM_IRON), 1);        // from the molten iron
    world_destroy(w);
}

void test_lava_boils_water(void) {
    // Lava poured onto a shallow layer of water boils some of it into steam, and freezes into
    // rock. (Into a deep pool, the same lava only warms the water: its heat spreads too far.)
    World *w = test_world_box(128, 128, 86);
    fill(w, 1, 120, 126, 126, ELEM_WATER);
    fill(w, 40, 60, 87, 79, ELEM_LAVA);
    int64_t lava = test_count(w, ELEM_LAVA);
    sim_run_ticks(w, 1200);
    EXPECT(test_count(w, ELEM_STEAM) > 0 || test_mass(w, ELEM_STEAM) > 0.0);
    EXPECT(test_count(w, ELEM_ROCK) > lava / 4); // much of it froze
    EXPECT_EQ(test_count(w, ELEM_LAVA) + test_count(w, ELEM_ROCK), lava);
    world_destroy(w);
}

void test_oil_floats_on_water(void) {
    // Oil (870 kg/m^3) under water rises through it and ends on top.
    World *w = test_world_box(64, 128, 87);
    fill(w, 1, 100, 62, 126, ELEM_OIL);
    fill(w, 1, 60, 62, 99, ELEM_WATER);
    int64_t oil = test_count(w, ELEM_OIL), water = test_count(w, ELEM_WATER);
    sim_run_ticks(w, 2400);
    EXPECT_EQ(test_count(w, ELEM_OIL), oil);
    EXPECT_EQ(test_count(w, ELEM_WATER), water);
    EXPECT(test_mean_y(w, ELEM_OIL) + 20.0 < test_mean_y(w, ELEM_WATER));
    world_destroy(w);
}

void test_oil_manometer(void) {
    // A U-tube of water with a 60-cell column of oil poured into its left arm. Liquids push each
    // other by weight: the water under the oil stands lower than in the right arm by
    // 60 x 870 / 1000 = 52 cells (a manometer). Nothing is lost.
    World *w = test_world_box(128, 192, 88);
    fill(w, 1, 1, 126, 190, ELEM_ROCK);
    fill(w, 20, 10, 39, 180, ELEM_VOID);  // left arm
    fill(w, 60, 10, 79, 180, ELEM_VOID);  // right arm
    fill(w, 20, 161, 79, 180, ELEM_VOID); // channel between them
    fill(w, 20, 120, 39, 180, ELEM_WATER);
    fill(w, 60, 120, 79, 180, ELEM_WATER);
    fill(w, 40, 161, 59, 180, ELEM_WATER);
    fill(w, 20, 60, 39, 119, ELEM_OIL);
    int64_t water = test_count(w, ELEM_WATER), oil = test_count(w, ELEM_OIL);
    sim_run_ticks(w, 3000);
    EXPECT_EQ(test_count(w, ELEM_WATER), water);
    EXPECT_EQ(test_count(w, ELEM_OIL), oil);
    int left = -1, right = -1;
    for (int y = 10; y <= 180 && (left < 0 || right < 0); y++) {
        if (left < 0 && test_cell_is(w, 30, y, ELEM_WATER)) {
            left = y;
        }
        if (right < 0 && test_cell_is(w, 70, y, ELEM_WATER)) {
            right = y;
        }
    }
    int64_t oil_right = 0;
    for (int y = 10; y <= 160; y++) {
        for (int x = 60; x <= 79; x++) {
            oil_right += test_cell_is(w, x, y, ELEM_OIL);
        }
    }
    EXPECT(left > 0 && right > 0);
    EXPECT(abs((left - right) - 52) <= 3);
    EXPECT(oil_right < oil / 20);
    world_destroy(w);
}
