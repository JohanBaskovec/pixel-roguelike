#include "test.h"

#include "core/jobs.h"
#include "sim/edit.h"
#include "sim/particle.h"
#include "sim/sim.h"

#include <math.h>
#include <string.h>

void test_particle_flies_ballistic(void);
void test_deep_reservoir_jets_further(void);
void test_fast_landing_splashes(void);
void test_jets_same_for_any_thread_count(void);
void test_buried_particle_lands_further_or_is_lost(void);

void test_particle_flies_ballistic(void) {
    // A drop thrown sideways at 2 cells/tick from 100 cells up lands where a ballistic arc says:
    // it falls 100 - 0.5 cells in t = sqrt(2 h / g) ticks.
    World *w = test_world_box(512, 128, 61);
    Particle p = {10.5f, 20.5f, 2.0f, 0.0f, {ELEM_WATER, 0, 0, 0}, 293.15f};
    EXPECT(particle_add(w, &p));
    int ticks = 0;
    while (w->particle_count > 0 && ticks < 1000) {
        sim_tick(w, NULL);
        ticks++;
    }
    EXPECT_EQ(w->particle_count, 0);
    int landed_x = -1;
    for (int x = 1; x < 511; x++) {
        if (test_cell_is(w, x, 126, ELEM_WATER)) {
            landed_x = x;
        }
    }
    double t = sqrt(2.0 * (126.0 - 20.0) / (double)PARTICLE_GRAVITY);
    double expect = 10.5 + 2.0 * t;
    EXPECT(landed_x > 0 && fabs((double)landed_x - expect) < 3.0);
    EXPECT_EQ(test_count(w, ELEM_WATER), 1);
    world_destroy(w);
}

// A reservoir of water `depth` cells deep on a shelf (floor at y = 170), behind a wall with a
// one-cell hole at its foot, and a deep pit beyond (floor at y = 254), so the water that leaves
// never connects back to the hole. Returns the water outside the reservoir after `ticks`, and in
// *reach the farthest column the water got to.
static int64_t drain(int depth, int ticks, int *reach) {
    World *w = test_world_box(512, 256, 62);
    Edit shelf = {1, 171, 60, 254, 0, EDIT_SQUARE, ELEM_ROCK, 0, EDIT_PAINT, 0.0f};
    for (int y = 171; y <= 254; y++) {
        shelf.y0 = shelf.y1 = y;
        sim_apply_edit(w, &shelf);
    }
    Edit wall = {60, 1, 60, 170, 0, EDIT_SQUARE, ELEM_ROCK, 0, EDIT_PAINT, 0.0f};
    sim_apply_edit(w, &wall);
    Edit hole = {60, 170, 60, 170, 0, EDIT_SQUARE, ELEM_VOID, 0, EDIT_PAINT, 0.0f};
    sim_apply_edit(w, &hole);
    for (int y = 171 - depth; y <= 170; y++) {
        Edit row = {1, y, 59, y, 0, EDIT_SQUARE, ELEM_WATER, 0, EDIT_PAINT, 0.0f};
        sim_apply_edit(w, &row);
    }
    *reach = 0;
    for (int t = 0; t < ticks; t++) {
        sim_tick(w, NULL);
        for (int i = 0; i < w->particle_count; i++) {
            *reach = PR_MAX(*reach, (int)w->particles[i].x);
        }
    }
    int64_t out = 0;
    for (int i = 0; i < w->particle_count; i++) {
        out += w->particles[i].x >= 61.0f;
    }
    for (int y = 1; y < 255; y++) {
        for (int x = 61; x < 511; x++) {
            if (test_cell_is(w, x, y, ELEM_WATER)) {
                out++;
                *reach = PR_MAX(*reach, x);
            }
        }
    }
    world_destroy(w);
    return out;
}

void test_deep_reservoir_jets_further(void) {
    // Torricelli: water leaves a hole at sqrt(2 g depth): 150 cells deep, 4.1 cells/tick (and as
    // many cells per tick through the hole); 25 deep, 1.65. The deep reservoir drains faster and
    // its jet reaches further.
    int reach_deep, reach_shallow;
    int64_t deep = drain(150, 120, &reach_deep);
    int64_t shallow = drain(25, 120, &reach_shallow);
    EXPECT(deep > 2 * shallow);
    EXPECT(deep > 300 && deep < 600);       // about 4 per tick for 120 ticks
    EXPECT(shallow > 120 && shallow < 280); // about 1.65 per tick
    EXPECT(reach_deep > reach_shallow + 20);
}

void test_fast_landing_splashes(void) {
    // A blob dropped from high up hits the floor fast: some of it splashes off as particles.
    // Nothing is lost, and everything lands again.
    World *w = test_world_box(256, 256, 63);
    Edit e = {128, 30, 128, 30, 6, EDIT_CIRCLE, ELEM_WATER, 0, EDIT_PAINT, 0.0f};
    int64_t n = sim_apply_edit(w, &e);
    uint32_t launched = 0;
    for (int t = 0; t < 1200; t++) {
        SimStats st;
        sim_tick(w, &st);
        launched += st.launched;
        EXPECT_EQ(test_count(w, ELEM_WATER), n);
    }
    EXPECT(launched > 5);
    EXPECT_EQ(w->particle_count, 0);
    world_destroy(w);
}

static World *jet_world(JobPool *pool) {
    World *w = test_world_box(256, 192, 64);
    world_set_job_pool(w, pool);
    Edit wall = {60, 1, 60, 190, 0, EDIT_SQUARE, ELEM_ROCK, 0, EDIT_PAINT, 0.0f};
    sim_apply_edit(w, &wall);
    Edit hole = {60, 150, 60, 152, 0, EDIT_SQUARE, ELEM_VOID, 0, EDIT_PAINT, 0.0f};
    sim_apply_edit(w, &hole);
    for (int y = 40; y <= 190; y++) {
        Edit row = {1, y, 59, y, 0, EDIT_SQUARE, ELEM_WATER, 0, EDIT_PAINT, 0.0f};
        sim_apply_edit(w, &row);
    }
    sim_run_ticks(w, 300);
    return w;
}

void test_jets_same_for_any_thread_count(void) {
    JobPool *pool = jobs_create(4);
    World *a = jet_world(NULL);
    World *b = jet_world(pool);
    size_t n = (size_t)a->width * a->height;
    EXPECT(memcmp(a->cells, b->cells, n * sizeof(Cell)) == 0);
    EXPECT_EQ(a->particle_count, b->particle_count);
    EXPECT(memcmp(a->particles, b->particles, (size_t)a->particle_count * sizeof(Particle)) == 0);
    world_destroy(a);
    world_destroy(b);
    jobs_destroy(pool);
}

// A particle buried where nothing within LAND_SEARCH is free (D-051) waits PARTICLE_WAIT_TICKS,
// then lands in the nearest free cell up to PARTICLE_WAIT_SEARCH away, or is lost: it never stays
// in the list for ever.
void test_buried_particle_lands_further_or_is_lost(void) {
    for (int pocket = 0; pocket < 2; pocket++) {
        World *w = test_world_box(128, 128, 9);
        for (int y = 1; y <= 126; y++) {
            Edit ed = {1, y, 126, y, 0, EDIT_SQUARE, ELEM_ROCK, 0, EDIT_PAINT, 0.0f};
            sim_apply_edit(w, &ed);
        }
        if (pocket) {
            Edit hole = {84, 64, 84, 64, 0, EDIT_SQUARE, ELEM_VOID, 0, EDIT_PAINT, 0.0f};
            sim_apply_edit(w, &hole); // 20 cells to the right
        }
        Cell blood = {ELEM_BLOOD, 0, 0, 0};
        Particle p = {64.5f, 64.5f, 0.0f, 0.0f, blood, T_ROOM};
        EXPECT(particle_add(w, &p));
        sim_run_ticks(w, PARTICLE_WAIT_TICKS - 5);
        EXPECT_EQ(w->particle_count, 1); // still waiting
        sim_run_ticks(w, 10);
        EXPECT_EQ(w->particle_count, 0);
        EXPECT_EQ(test_count(w, ELEM_BLOOD), pocket ? 1 : 0);
        world_destroy(w);
    }
}
