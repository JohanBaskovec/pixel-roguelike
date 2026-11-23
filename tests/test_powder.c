#include "test.h"

#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/sim.h"

void test_sand_piles_up(void);
void test_sand_sinks_in_water(void);
void test_digging_makes_rubble(void);
void test_creature_stands_on_sand(void);

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT};
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
    Edit dig = {50, 55, 50, 55, 6, EDIT_CIRCLE, ELEM_VOID, 0, EDIT_DIG}; // reaches the underside
    sim_apply_edit(w, &dig);
    Edit dig2 = {107, 72, 107, 72, 3, EDIT_SQUARE, ELEM_VOID, 0, EDIT_DIG}; // through the dirt
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
