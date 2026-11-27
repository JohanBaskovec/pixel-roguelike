#include "test.h"

#include "core/jobs.h"
#include "core/rng.h"
#include "sim/edit.h"
#include "sim/particle.h"
#include "sim/rigid.h"
#include "sim/sim.h"

#include <math.h>
#include <string.h>

void test_log_falls_and_rests(void);
void test_log_tips_off_a_ledge(void);
void test_log_is_cut_in_two(void);
void test_bullet_cuts_a_log(void);
void test_rigid_same_for_any_thread_count(void);
void test_rigid_survives_a_barrage(void);
void test_burning_log_burns_away(void);
void test_slivers_crumble_to_sawdust(void);

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT, 0.0f, 0.0f};
        sim_apply_edit(w, &ed);
    }
}

static World *room(uint64_t seed) {
    World *w = test_world_box(128, 128, seed);
    fill(w, 1, 1, 126, 126, EDIT_AIR);
    return w;
}

static double total_gas(const World *w) {
    double m = 0.0;
    for (int s = 0; s < GAS_SPECIES; s++) {
        m += test_mass(w, g_species_elem[s]);
    }
    return m;
}

void test_log_falls_and_rests(void) {
    // A log (24 x 4 pixels) dropped in air falls, lands flat, and sleeps on the floor. Its wood
    // is all there, and no gas was lost on the way.
    World *w = room(150);
    double gas0 = total_gas(w);
    int li = rigid_spawn_box(w, 60.0f, 40.0f, 24, 4, 0.0f);
    EXPECT(li >= 0);
    EXPECT_EQ(test_count(w, ELEM_WOOD), 96);
    sim_run_ticks(w, 300);
    const Rigid *r = &w->rigids[li];
    EXPECT(r->active);
    EXPECT(r->asleep);
    EXPECT(fabsf(r->angle) < 0.05f);
    EXPECT(r->y > 122.0f && r->y < 126.0f); // lying on the floor (the bottom row is 126)
    EXPECT_EQ(test_count(w, ELEM_WOOD), 96);
    EXPECT(fabs(total_gas(w) - gas0) < 1e-6 * gas0 + 1e-9);
    world_destroy(w);
}

void test_log_tips_off_a_ledge(void) {
    // A log lying on a ledge with most of it over the edge pivots and falls: it turns (more than
    // a quarter turn on the way), and ends up on the floor below.
    World *w = room(151);
    fill(w, 1, 90, 40, 126, ELEM_ROCK); // a ledge, top at row 90
    int li = rigid_spawn_box(w, 56.0f, 88.0f - 2.0f, 36, 4, 0.0f); // x 38..74: 2 cells on the ledge
    EXPECT(li >= 0);
    const Rigid *r = &w->rigids[li];
    float turned = 0.0f;
    for (int t = 0; t < 400; t++) {
        sim_tick(w, NULL);
        turned = fmaxf(turned, fabsf(r->angle));
    }
    EXPECT(turned > 0.4f);
    EXPECT(r->y > 100.0f); // below the ledge
    EXPECT(r->asleep);
    world_destroy(w);
}

void test_log_is_cut_in_two(void) {
    // A log bridging two rock pillars is cut in the middle (cells erased): two bodies, each with
    // its own half of the pixels; both fall (to the floor) and sleep there.
    World *w = room(152);
    fill(w, 20, 100, 30, 126, ELEM_ROCK);
    fill(w, 70, 100, 80, 126, ELEM_ROCK);
    int li = rigid_spawn_box(w, 50.0f, 98.0f - 2.0f, 60, 4, 0.0f); // x 20..80 on the pillars
    EXPECT(li >= 0);
    sim_run_ticks(w, 60);
    EXPECT(w->rigids[li].asleep); // it rests on the pillars
    EXPECT_EQ(test_count(w, ELEM_WOOD), 240);
    fill(w, 49, 90, 51, 110, ELEM_VOID); // the cut: 3 columns through the log
    sim_run_ticks(w, 40);
    int bodies = 0, pixels = 0;
    for (int i = 0; i < w->rigid_count; i++) {
        if (w->rigids[i].active) {
            bodies++;
            pixels += w->rigids[i].pixels;
        }
    }
    EXPECT_EQ(bodies, 2);
    EXPECT_EQ(pixels, 240 - 12);
    sim_run_ticks(w, 300);
    pixels = 0;
    for (int i = 0; i < w->rigid_count; i++) {
        pixels += w->rigids[i].active ? w->rigids[i].pixels : 0;
    }
    EXPECT_EQ(pixels, 240 - 12); // no pixel lost in the fall
    EXPECT(test_count(w, ELEM_WOOD) > 200 && test_count(w, ELEM_WOOD) < 260);
    world_destroy(w);
}

void test_bullet_cuts_a_log(void) {
    // A bullet through a log bridging two pillars cuts a kerf: the log falls in two pieces, and
    // the bullet's blow turns them a little.
    World *w = room(153);
    fill(w, 20, 100, 30, 126, ELEM_ROCK);
    fill(w, 70, 100, 80, 126, ELEM_ROCK);
    int li = rigid_spawn_box(w, 50.0f, 98.0f - 2.0f, 60, 4, 0.0f);
    EXPECT(li >= 0);
    sim_run_ticks(w, 60);
    Cell bullet = {ELEM_BULLET, 0, 0, 0};
    Particle b = {50.5f, 80.5f, 0.0f, 300.0f, bullet, T_ROOM};
    EXPECT(particle_add(w, &b));
    sim_run_ticks(w, 20);
    int bodies = 0;
    for (int i = 0; i < w->rigid_count; i++) {
        bodies += w->rigids[i].active;
    }
    EXPECT_EQ(bodies, 2);
    EXPECT(test_count(w, ELEM_WOOD) < 240);
    sim_run_ticks(w, 300);
    for (int i = 0; i < w->rigid_count; i++) {
        if (w->rigids[i].active) {
            EXPECT(w->rigids[i].y > 110.0f); // both halves came down
        }
    }
    world_destroy(w);
}

static void ledge_and_cut(World *w) {
    fill(w, 1, 90, 40, 126, ELEM_ROCK);
    rigid_spawn_box(w, 56.0f, 86.0f, 36, 4, 0.0f); // tips off the ledge
    fill(w, 90, 100, 100, 126, ELEM_ROCK);
    fill(w, 110, 100, 120, 126, ELEM_ROCK);
    rigid_spawn_box(w, 105.0f, 96.0f, 40, 4, 0.0f); // bridges two pillars
    sim_run_ticks(w, 50);
    fill(w, 104, 90, 106, 110, ELEM_VOID); // cut
    sim_run_ticks(w, 150);
}

void test_rigid_same_for_any_thread_count(void) {
    // Bodies move serially, but the gas and heat they disturb run in parallel: the result is
    // bit-identical on 1 and 4 threads.
    JobPool *pool = jobs_create(4);
    World *a = room(154), *b = room(154);
    world_set_job_pool(b, pool);
    ledge_and_cut(a);
    ledge_and_cut(b);
    size_t n = (size_t)a->width * a->height;
    EXPECT(memcmp(a->cells, b->cells, n * sizeof(Cell)) == 0);
    EXPECT(memcmp(a->temp, b->temp, n * sizeof(float)) == 0);
    EXPECT(memcmp(a->gas, b->gas, n * sizeof(GasMix)) == 0);
    EXPECT_EQ(a->rigid_count, b->rigid_count);
    world_destroy(a);
    world_destroy(b);
    jobs_destroy(pool);
}

void test_rigid_survives_a_barrage(void) {
    // Bullets at random places through a heap of logs and boxes: bodies split again and again,
    // fall, tumble and pile up. Afterwards every wood cell belongs to exactly one body, no body is
    // empty, and the pixels they own were never more than the wood there was.
    World *w = room(155);
    Rng rng = rng_from_seed(5);
    for (int k = 0; k < 6; k++) {
        rigid_spawn_box(w, 40.0f + (float)(k % 3) * 20.0f, 100.0f - (float)(k / 3) * 8.0f, 36, 5, 0.0f);
    }
    rigid_spawn_box(w, 100.0f, 118.0f, 12, 12, 0.0f);
    int64_t wood0 = test_count(w, ELEM_WOOD);
    for (int shot = 0; shot < 24; shot++) {
        Cell bullet = {ELEM_BULLET, 0, 0, 0};
        float x = 25.0f + (float)rng_range(&rng, 90), vx = ((float)rng_range(&rng, 200) - 100.0f) * 0.02f;
        Particle b = {x, 70.0f, vx, 300.0f, bullet, T_ROOM};
        particle_add(w, &b);
        sim_run_ticks(w, 12);
    }
    sim_run_ticks(w, 400);
    int64_t cells = 0;
    int bodies = 0;
    for (int i = 0; i < w->rigid_count; i++) {
        const Rigid *r = &w->rigids[i];
        if (!r->active) {
            continue;
        }
        bodies++;
        EXPECT(r->pixels >= RIGID_MIN_PIECE);
        EXPECT(r->ncells > 0);
        cells += r->ncells;
        for (int k = 0; k < r->ncells; k++) {
            ElementId e = w->cells[r->cell_idx[k]].element;
            EXPECT(e == ELEM_WOOD || e == ELEM_BURNING_WOOD);
        }
    }
    EXPECT(bodies >= 6);
    int64_t wood = test_count(w, ELEM_WOOD) + test_count(w, ELEM_BURNING_WOOD);
    EXPECT_EQ(wood, cells); // no stray wood cell, none counted twice
    EXPECT(wood <= wood0 + wood0 / 10);
    world_destroy(w);
}

void test_burning_log_burns_away(void) {
    // A log lit at one cell burns: its heat and fire travel with it, burnt-away pixels leave the
    // body (which may fall in pieces), and every wood cell still belongs to a body.
    World *w = room(156);
    int li = rigid_spawn_box(w, 60.0f, 124.0f, 30, 5, 0.0f);
    EXPECT(li >= 0);
    sim_run_ticks(w, 40);
    int before = w->rigids[li].pixels;
    w->temp[world_index(w, 60, 122)] = 700.0f; // a cell of the top face (air reaches it)
    world_wake_heat(w, 40, 110, 80, 126);
    sim_run_ticks(w, 700);
    int pixels = 0;
    int64_t cells = 0;
    for (int i = 0; i < w->rigid_count; i++) {
        if (w->rigids[i].active) {
            pixels += w->rigids[i].pixels;
            cells += w->rigids[i].ncells;
        }
    }
    EXPECT(pixels < before);                                  // some burnt away
    EXPECT(test_mass(w, ELEM_CO2) > 0.003);                   // making carbon dioxide
    EXPECT_EQ(test_count(w, ELEM_WOOD) + test_count(w, ELEM_BURNING_WOOD), cells);
    world_destroy(w);
}

void test_slivers_crumble_to_sawdust(void) {
    // Cut into single pixels, a thin log does not vanish: each pixel becomes sawdust, which falls
    // to the floor like sand (the cell count is kept).
    World *w = room(157);
    int li = rigid_spawn_box(w, 60.5f, 60.5f, 5, 1, 0.0f); // x 58..62, row 60
    EXPECT(li >= 0);
    EXPECT_EQ(test_count(w, ELEM_WOOD), 5);
    fill(w, 59, 55, 59, 65, ELEM_VOID);
    fill(w, 61, 55, 61, 65, ELEM_VOID);
    sim_run_ticks(w, 4);
    EXPECT_EQ(test_count(w, ELEM_SAWDUST), 3);
    EXPECT_EQ(test_count(w, ELEM_WOOD), 0);
    int bodies = 0;
    for (int i = 0; i < w->rigid_count; i++) {
        bodies += w->rigids[i].active;
    }
    EXPECT_EQ(bodies, 0);
    sim_run_ticks(w, 150);
    EXPECT_EQ(test_count(w, ELEM_SAWDUST), 3);
    EXPECT(test_mean_y(w, ELEM_SAWDUST) > 124.0); // on the floor
    world_destroy(w);
}
