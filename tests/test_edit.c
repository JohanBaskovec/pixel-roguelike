#include "test.h"

#include "sim/edit.h"

void test_edit_single_pixel(void);
void test_edit_circle_area(void);
void test_edit_stroke_has_no_gaps(void);
void test_edit_only_void(void);
void test_edit_erase(void);

void test_edit_single_pixel(void) {
    World *w = test_world_box(64, 64, 1);
    Edit e = {10, 20, 10, 20, 0, EDIT_CIRCLE, ELEM_IRON, 0};
    EXPECT_EQ(sim_apply_edit(w, &e), 1);
    EXPECT(test_cell_is(w, 10, 20, ELEM_IRON));
    e.shape = EDIT_SQUARE;
    e.x0 = e.x1 = 30;
    EXPECT_EQ(sim_apply_edit(w, &e), 1);
    world_destroy(w);
}

void test_edit_circle_area(void) {
    World *w = test_world_box(128, 128, 1);
    Edit e = {64, 64, 64, 64, 20, EDIT_CIRCLE, ELEM_ROCK, 0};
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
    Edit e = {5, 5, 120, 90, 0, EDIT_CIRCLE, ELEM_IRON, 0};
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
    Edit wall = {0, 0, 0, 0, 3, EDIT_CIRCLE, ELEM_WATER, 1};
    EXPECT(sim_apply_edit(w, &wall) > 0);
    EXPECT(test_cell_is(w, 0, 0, ELEM_DIRT));
    EXPECT(test_cell_is(w, 1, 1, ELEM_WATER));
    EXPECT_EQ(test_count(w, ELEM_DIRT), 64 * 4 - 4);
    world_destroy(w);
}

void test_edit_erase(void) {
    World *w = test_world_box(64, 64, 1);
    Edit e = {0, 32, 0, 32, 4, EDIT_CIRCLE, ELEM_VOID, 0};
    sim_apply_edit(w, &e);
    EXPECT(test_cell_is(w, 0, 32, ELEM_VOID));
    world_destroy(w);
}
