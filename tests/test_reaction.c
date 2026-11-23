#include "test.h"

#include "core/jobs.h"
#include "sim/sim.h"

#include <math.h>
#include <string.h>

void test_hydrogen_burns_in_oxygen(void);
void test_cold_hydrogen_does_not_burn(void);
void test_burning_same_for_any_thread_count(void);

// A sealed room of hydrogen and oxygen at 1 atm, mixed 2:1 by volume (stoichiometric), as
// alternating cells.
static World *knallgas(uint64_t seed) {
    World *w = test_world_box(128, 64, seed);
    for (int y = 1; y < 63; y++) {
        for (int x = 1; x < 127; x++) {
            ElementId e = (x + 2 * y) % 3 == 0 ? ELEM_OXYGEN : ELEM_HYDROGEN;
            world_set_cell(w, x, y, e, (uint32_t)(x * 31 + y * 7));
        }
    }
    world_wake_rect(w, 0, 0, 127, 63);
    world_wake_heat(w, 0, 0, 127, 63);
    return w;
}

void test_hydrogen_burns_in_oxygen(void) {
    // Lit at one spot (a few cells at 1000 K), the flame spreads through the mixture: hydrogen
    // and oxygen turn into steam, 8 kg of oxygen per kg of hydrogen, and the steam is hot. Mass is
    // conserved exactly (counted before the steam has cooled enough to condense).
    World *w = knallgas(91);
    double h2 = test_mass(w, ELEM_HYDROGEN), o2 = test_mass(w, ELEM_OXYGEN);
    for (int y = 30; y <= 33; y++) {
        for (int x = 60; x <= 63; x++) {
            w->temp[world_index(w, x, y)] = 1000.0f;
        }
    }
    world_wake_heat(w, 55, 25, 68, 38);
    uint32_t reactions = 0;
    float hottest = 0.0f;
    for (int t = 0; t < 240; t++) {
        SimStats st;
        sim_tick(w, &st);
        reactions += st.reactions;
    }
    for (int i = 0; i < w->width * w->height; i++) {
        if (w->cells[i].element == ELEM_STEAM) {
            hottest = PR_MAX(hottest, w->temp[i]);
        }
    }
    double h2_left = test_mass(w, ELEM_HYDROGEN), o2_left = test_mass(w, ELEM_OXYGEN);
    double steam = test_mass(w, ELEM_STEAM) +
                   (double)test_count(w, ELEM_WATER) * (double)g_elem_mass[ELEM_WATER];
    double burned = h2 - h2_left;
    EXPECT(reactions > 1000);
    EXPECT(burned > 0.5 * h2); // most of it burned
    EXPECT(fabs((o2 - o2_left) - 7.937 * burned) < 0.01 * 7.937 * burned);
    EXPECT_EQ(test_count(w, ELEM_WATER), 0); // still too hot to condense
    EXPECT(fabs(steam - (burned + (o2 - o2_left))) < 1e-3 * steam);
    EXPECT(hottest > 1500.0f && hottest <= FLAME_MAX_K + 1.0f);
    world_destroy(w);
}

void test_cold_hydrogen_does_not_burn(void) {
    World *w = knallgas(92);
    double h2 = test_mass(w, ELEM_HYDROGEN);
    uint32_t reactions = 0;
    for (int t = 0; t < 600; t++) {
        SimStats st;
        sim_tick(w, &st);
        reactions += st.reactions;
    }
    EXPECT_EQ(reactions, 0u);
    EXPECT(fabs(test_mass(w, ELEM_HYDROGEN) - h2) < 1e-4 * h2);
    world_destroy(w);
}

void test_burning_same_for_any_thread_count(void) {
    JobPool *pool = jobs_create(4);
    World *a = knallgas(93), *b = knallgas(93);
    world_set_job_pool(b, pool);
    World *ws[2] = {a, b};
    for (int k = 0; k < 2; k++) {
        ws[k]->temp[world_index(ws[k], 60, 30)] = 1000.0f;
        world_wake_heat(ws[k], 59, 29, 61, 31);
        sim_run_ticks(ws[k], 120);
    }
    size_t n = (size_t)a->width * a->height;
    EXPECT(memcmp(a->cells, b->cells, n * sizeof(Cell)) == 0);
    EXPECT(memcmp(a->mass, b->mass, n * sizeof(float)) == 0);
    EXPECT(memcmp(a->temp, b->temp, n * sizeof(float)) == 0);
    world_destroy(a);
    world_destroy(b);
    jobs_destroy(pool);
}
