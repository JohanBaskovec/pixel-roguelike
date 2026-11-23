#include "test.h"

#include "core/jobs.h"
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
void test_poured_water_spreads(void);
void test_falling_stream_has_no_chunk_bulges(void);
void test_pour_against_wall_makes_no_tower(void);
void test_same_result_for_any_thread_count(void);
void test_u_tube_balances(void);
void test_water_flows_through_hole_until_level(void);

void test_solids_never_move(void) {
    World *w = test_world_box(64, 64, 1);
    Edit e = {20, 10, 40, 10, 2, EDIT_CIRCLE, ELEM_IRON, 0, EDIT_PAINT};
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
    Edit e = {32, 12, 32, 12, 8, EDIT_CIRCLE, ELEM_WATER, 0, EDIT_PAINT};
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
    Edit fill = {32, 32, 32, 32, 40, EDIT_SQUARE, ELEM_OXYGEN, 1, EDIT_PAINT};
    sim_apply_edit(w, &fill);
    Edit water = {32, 8, 32, 8, 5, EDIT_CIRCLE, ELEM_WATER, 0, EDIT_PAINT};
    sim_apply_edit(w, &water);
    sim_run_ticks(w, 600);
    EXPECT(test_mean_y(w, ELEM_WATER) > 58.0);
    world_destroy(w);
}

void test_gas_expands_into_void(void) {
    World *w = test_world_box(64, 64, 4);
    Edit e = {32, 56, 32, 56, 4, EDIT_CIRCLE, ELEM_OXYGEN, 0, EDIT_PAINT};
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
    Edit a = {1, 1, 62, 31, 0, EDIT_SQUARE, top, 0, EDIT_PAINT};
    Edit b = {1, 32, 62, 62, 0, EDIT_SQUARE, bottom, 0, EDIT_PAINT};
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
        Edit e = {20 + i * 20, 30, 30 + i * 18, 90, 6, EDIT_CIRCLE, els[i], 0, EDIT_PAINT};
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
            Edit ed = {20 + t / 4, 20, 40 + t / 5, 30, 5, EDIT_CIRCLE, e, 0, EDIT_PAINT};
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
    Edit row = {1, 0, 126, 0, 0, EDIT_SQUARE, ELEM_WATER, 0, EDIT_PAINT};
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

    // A poured pool settles completely: no cell is updated any more (no surface shimmer).
    w = test_world_box(128, 128, 8);
    Edit e = {64, 40, 64, 40, 25, EDIT_CIRCLE, ELEM_WATER, 0, EDIT_PAINT};
    sim_apply_edit(w, &e);
    sim_run_ticks(w, 3000);
    sim_tick(w, &s);
    EXPECT_EQ(s.cells_visited, 0);
    EXPECT_EQ(s.chunks_active, 0);
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

void test_poured_water_spreads(void) {
    // Hold the brush still, like a player pouring. The water must spread across the floor, not
    // pile into a stepped mound. (Before surface cells could look through their own liquid,
    // this left 21% of the floor dry and a peak of 25 cells; flat is about 10.)
    World *w = test_world_box(512, 192, 10);
    for (int t = 0; t < 400; t++) {
        Edit e = {256, 20, 256, 20, 6, EDIT_CIRCLE, ELEM_WATER, 0, EDIT_PAINT};
        sim_apply_edit(w, &e);
        sim_tick(w, NULL);
    }
    sim_run_ticks(w, 300);
    int covered = 0, max_h = 0;
    for (int x = 1; x < 511; x++) {
        covered += test_cell_is(w, x, 190, ELEM_WATER);
        int h = 0;
        for (int y = 1; y < 191; y++) {
            h += test_cell_is(w, x, y, ELEM_WATER);
        }
        max_h = PR_MAX(max_h, h);
    }
    int flat = (int)(test_count(w, ELEM_WATER) / 510);
    int min_h = 1000;
    for (int x = 1; x < 511; x++) {
        int h = 0;
        for (int y = 1; y < 191; y++) {
            h += test_cell_is(w, x, y, ELEM_WATER);
        }
        min_h = PR_MIN(min_h, h);
    }
    EXPECT(covered == 510);
    EXPECT(max_h <= flat + 1);
    // Water pressure levels the whole basin to within one cell.
    EXPECT(max_h - min_h <= 1);
    world_destroy(w);
}

void test_falling_stream_has_no_chunk_bulges(void) {
    // A poured stream used to bulge below every chunk border: where the chunk below is scanned
    // later, falling cells "landed" on the stream's unmoved leading cells and spread sideways.
    // Compare the stream's average width near chunk borders with its width mid-chunk.
    World *w = test_world_box(192, 512, 11);
    static double width[512];
    int samples = 0;
    for (int t = 0; t < 320; t++) {
        Edit e = {96, 20, 96, 20, 6, EDIT_CIRCLE, ELEM_WATER, 0, EDIT_PAINT};
        sim_apply_edit(w, &e);
        sim_tick(w, NULL);
        if (t >= 170) {
            samples++;
            for (int y = 64; y < 448; y++) {
                for (int x = 1; x < 191; x++) {
                    width[y] += test_cell_is(w, x, y, ELEM_WATER);
                }
            }
        }
    }
    double near = 0.0, mid = 0.0;
    int n_near = 0, n_mid = 0;
    for (int y = 64; y < 448; y++) {
        int d = y & (CHUNK_SIZE - 1);
        int dist = d < CHUNK_SIZE / 2 ? d : CHUNK_SIZE - d;
        if (dist <= 4) {
            near += width[y];
            n_near++;
        } else if (dist >= 16) {
            mid += width[y];
            n_mid++;
        }
    }
    double ratio = (near / n_near) / (mid / n_mid);
    EXPECT(samples > 0);
    EXPECT(ratio < 1.2); // was about 1.6 with the bulges
    world_destroy(w);
}

void test_pour_against_wall_makes_no_tower(void) {
    // Pouring right beside a wall used to stack settled water into a standing tower against the
    // wall: it could only drain from its top. Count settled (not falling) water well above the
    // average pool level near the wall.
    World *w = test_world_box(1024, 384, 12);
    for (int t = 0; t < 2400; t++) {
        Edit e = {990, 30, 990, 30, 8, EDIT_CIRCLE, ELEM_WATER, 0, EDIT_PAINT};
        sim_apply_edit(w, &e);
        sim_tick(w, NULL);
    }
    int flat = (int)(test_count(w, ELEM_WATER) / 1022);
    int pool_top = 382 - flat;
    int stacked = 0;
    for (int y = 1; y < pool_top - 40; y++) {
        for (int x = 940; x < 1023; x++) {
            Cell c = w->cells[world_index(w, x, y)];
            stacked += c.element == ELEM_WATER && (c.aux & 0x7F) == 0;
        }
    }
    EXPECT(stacked < 200);
    world_destroy(w);
}

static World *scripted_world_threads(uint64_t seed, JobPool *pool) {
    World *w = test_world_box(384, 256, seed);
    world_set_job_pool(w, pool);
    for (int t = 0; t < 400; t++) {
        if (t % 20 == 0) {
            ElementId e = (ElementId)(ELEM_WATER + (t / 20) % 4);
            Edit ed = {30 + t / 2, 20, 60 + t / 3, 40, 8, EDIT_CIRCLE, e, 0, EDIT_PAINT};
            sim_apply_edit(w, &ed);
        }
        sim_tick(w, NULL);
    }
    return w;
}

void test_same_result_for_any_thread_count(void) {
    // The world after the same seed and edits must be bit-identical whether the simulation ran
    // on 1 thread or several (docs/ARCHITECTURE.md -> Determinism).
    JobPool *pool4 = jobs_create(4);
    JobPool *pool3 = jobs_create(3);
    World *a = scripted_world_threads(77, NULL);
    World *b = scripted_world_threads(77, pool4);
    World *c = scripted_world_threads(77, pool3);
    size_t n = (size_t)a->width * a->height;
    EXPECT(jobs_thread_count(pool4) == 4);
    EXPECT(memcmp(a->cells, b->cells, n * sizeof(Cell)) == 0);
    EXPECT(memcmp(a->temp, b->temp, n * sizeof(float)) == 0);
    EXPECT(memcmp(a->cells, c->cells, n * sizeof(Cell)) == 0);
    world_destroy(a);
    world_destroy(b);
    world_destroy(c);
    jobs_destroy(pool4);
    jobs_destroy(pool3);
}

// Height of the water surface (top row containing water) in columns x0..x1 (+y is down).
static int surface_y(const World *w, int x0, int x1) {
    for (int y = 0; y < w->height; y++) {
        for (int x = x0; x <= x1; x++) {
            if (test_cell_is(w, x, y, ELEM_WATER)) {
                return y;
            }
        }
    }
    return w->height;
}

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT};
        sim_apply_edit(w, &ed);
    }
}

void test_u_tube_balances(void) {
    // Two arms joined at the bottom (a dirt divider with a gap under it). Water starts high on the
    // left and low on the right; pressure must bring both surfaces level (within one cell).
    World *w = test_world_box(256, 192, 13);
    fill(w, 127, 1, 129, 160, ELEM_DIRT); // divider, open below y=160
    fill(w, 1, 60, 126, 190, ELEM_WATER);
    fill(w, 127, 161, 129, 190, ELEM_WATER);
    fill(w, 130, 150, 254, 190, ELEM_WATER);
    int left0 = surface_y(w, 1, 126), right0 = surface_y(w, 130, 254);
    EXPECT(right0 - left0 > 80);
    sim_run_ticks(w, 2500);
    int left = surface_y(w, 1, 126), right = surface_y(w, 130, 254);
    EXPECT(left > left0 + 30);
    EXPECT(right < right0 - 30);
    EXPECT(right - left <= 1 && left - right <= 1);
    world_destroy(w);
}

void test_water_flows_through_hole_until_level(void) {
    // A reservoir behind a wall with a small opening near the bottom: water must flow out through
    // it into the empty side until both sides are level (within one cell).
    World *w = test_world_box(256, 192, 14);
    fill(w, 96, 1, 99, 190, ELEM_DIRT);   // wall
    fill(w, 96, 176, 99, 183, ELEM_VOID); // 8-cell opening
    fill(w, 1, 60, 95, 190, ELEM_WATER);
    int64_t total = test_count(w, ELEM_WATER);
    sim_run_ticks(w, 3000);
    int left = surface_y(w, 1, 95), right = surface_y(w, 100, 254);
    EXPECT(right < w->height); // water got through
    EXPECT(right - left <= 1 && left - right <= 1);
    EXPECT_EQ(test_count(w, ELEM_WATER), total);
    world_destroy(w);
}
