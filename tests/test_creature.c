#include "test.h"

#include "core/jobs.h"
#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/sim.h"

#include <math.h>
#include <string.h>

void test_creature_falls_and_stands(void);
void test_creature_walks_and_steps_up(void);
void test_creature_stirs_gas_and_conserves_it(void);
void test_creature_floats_in_water(void);
void test_creature_breathes_at_real_rates(void);
void test_creature_suffocates_in_sealed_room(void);
void test_creature_body_heat(void);
void test_creatures_same_for_any_thread_count(void);
void test_walking_stirs_air(void);
void test_falling_sand_stirs_air(void);

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT};
        sim_apply_edit(w, &ed);
    }
}

void test_creature_falls_and_stands(void) {
    World *w = test_world_box(128, 128, 71);
    int id = creature_spawn(w, 40, 10);
    EXPECT(id == 0);
    EXPECT_EQ(test_count(w, ELEM_BODY), CREATURE_W * CREATURE_H);
    sim_run_ticks(w, 300);
    Creature *c = &w->creatures[id];
    EXPECT_EQ(c->y + CREATURE_H - 1, 126); // standing on the floor
    EXPECT(c->grounded);
    EXPECT_EQ(test_count(w, ELEM_BODY), CREATURE_W * CREATURE_H);
    world_destroy(w);
}

void test_creature_walks_and_steps_up(void) {
    // A step 3 cells high (15 cm) is walked up; a wall 24 cells high stops it.
    World *w = test_world_box(256, 128, 72);
    fill(w, 120, 124, 254, 126, ELEM_ROCK);
    fill(w, 200, 100, 254, 123, ELEM_ROCK);
    int id = creature_spawn(w, 20, 80);
    sim_run_ticks(w, 120);
    w->creatures[id].walk = 1;
    sim_run_ticks(w, 900);
    Creature *c = &w->creatures[id];
    EXPECT(c->x > 130);
    EXPECT_EQ(c->y + CREATURE_H - 1, 123); // on the step
    EXPECT(c->x + CREATURE_W <= 200);      // stopped by the wall
    world_destroy(w);
}

void test_creature_stirs_gas_and_conserves_it(void) {
    // Oxygen on the left, nitrogen on the right. Walking right carries the gas it walks into
    // around the body to behind it: nitrogen ends up on the oxygen side. No gas is created or
    // lost.
    World *w = test_world_box(256, 128, 73);
    fill(w, 1, 1, 127, 126, ELEM_OXYGEN);
    fill(w, 128, 1, 254, 126, ELEM_NITROGEN);
    int id = creature_spawn(w, 20, 91);
    EXPECT(id == 0);
    w->creatures[id].metabolism_scale = 0.0f; // no breathing here
    double o2 = test_mass(w, ELEM_OXYGEN), n2 = test_mass(w, ELEM_NITROGEN);
    w->creatures[id].walk = 1;
    sim_run_ticks(w, 480);
    int64_t n2_left = 0;
    for (int y = 1; y < 127; y++) {
        for (int x = 1; x < 128; x++) {
            n2_left += test_cell_is(w, x, y, ELEM_NITROGEN);
        }
    }
    EXPECT(w->creatures[id].x > 180);
    EXPECT(n2_left > 100);
    EXPECT(fabs(test_mass(w, ELEM_OXYGEN) - o2) < 1e-4 * o2);
    EXPECT(fabs(test_mass(w, ELEM_NITROGEN) - n2) < 1e-4 * n2);
    world_destroy(w);
}

void test_creature_floats_in_water(void) {
    // Dropped into a deep pool, it ends floating with its head out (bodies are a little lighter
    // than water, and it swims up when its head is under). Water is conserved.
    World *w = test_world_box(128, 192, 74);
    fill(w, 1, 100, 126, 190, ELEM_WATER);
    int64_t water = test_count(w, ELEM_WATER);
    int id = creature_spawn(w, 60, 20);
    sim_run_ticks(w, 1800);
    Creature *c = &w->creatures[id];
    EXPECT_EQ(test_count(w, ELEM_WATER), water);
    EXPECT(c->submerged > 0.3f);         // in the water
    EXPECT(c->y + CREATURE_H - 1 < 190); // not on the bottom
    EXPECT(!test_cell_is(w, c->x - 1, c->y + CREATURE_MOUTH, ELEM_WATER) ||
           !test_cell_is(w, c->x + CREATURE_W, c->y + CREATURE_MOUTH, ELEM_WATER)); // head out
    EXPECT(c->state != CREATURE_DEAD);
    world_destroy(w);
}

static World *air_room(int width, int height, uint64_t seed) {
    World *w = test_world_box(width, height, seed);
    Edit air = {1, 1, width - 2, 1, 0, EDIT_SQUARE, EDIT_AIR, 0, EDIT_PAINT};
    for (int y = 1; y <= height - 2; y++) {
        air.y0 = air.y1 = y;
        sim_apply_edit(w, &air);
    }
    return w;
}

void test_creature_breathes_at_real_rates(void) {
    // At rest: 250 mL/min of O2 used and 200 mL/min of CO2 made (0.333 and 0.368 g/min), taken
    // from and put into the air. Breathing sped up 10x here.
    World *w = air_room(128, 64, 75);
    int id = creature_spawn(w, 60, 27);
    w->creatures[id].metabolism_scale = 10.0f;
    double o2_0 = test_mass(w, ELEM_OXYGEN), co2_0 = test_mass(w, ELEM_CO2);
    int ticks = 257 * 5; // 5 breaths
    sim_run_ticks(w, ticks);
    Creature *c = &w->creatures[id];
    double minutes = ticks / 3600.0 * 10.0;
    EXPECT(fabs(c->o2_used - 0.333e-3 * minutes) < 0.02 * 0.333e-3 * minutes);
    EXPECT(fabs(c->co2_made - 0.368e-3 * minutes) < 0.02 * 0.368e-3 * minutes);
    EXPECT(fabs((o2_0 - test_mass(w, ELEM_OXYGEN)) - c->o2_used) < 1e-3 * o2_0);
    EXPECT(fabs((test_mass(w, ELEM_CO2) - co2_0) - c->co2_made) < 1e-3 * c->co2_made + 1e-9);
    // Not short of air (though at 10x the oxygen next to the mouth runs low: "impaired").
    EXPECT(c->state != CREATURE_DANGER);
    world_destroy(w);
}

void test_creature_suffocates_in_sealed_room(void) {
    // A sealed room of air (126 x 62 cells, 6.3 x 3.1 m, one cell deep) with one person at rest.
    // Breathed-out CO2 stays mixed in the air, so (as in real sealed rooms) it is the CO2 that
    // reaches the danger level (5 kPa) first: with 0.368 g/min, about 4 hours of game time for this
    // much air, while oxygen would take 7 hours to fall from 21 to 10 kPa. Sped up 200x here
    // (breathing stirs the room in proportion); the person reaches danger within 25% of the
    // earlier of the two, and then loses health. (In a smaller room the breath, warm, collects
    // near the head, and the person is in danger well before the room's average says so.)
    World *w = air_room(128, 64, 76);
    int id = creature_spawn(w, 60, 27);
    w->creatures[id].metabolism_scale = 200.0f;
    // Masses from the room's air volume (ideal gas at 20 degC): O2 to use before its partial
    // pressure falls to 10 kPa, and CO2 that makes 5 kPa.
    int64_t gas_cells = 0;
    for (int i = 0; i < w->width * w->height; i++) {
        gas_cells += g_elem_behavior[w->cells[i].element] == BEHAVIOR_GAS;
    }
    double v = (double)gas_cells * (double)CELL_VOLUME_M3, rt = (double)GAS_R * 293.15;
    double need = test_mass(w, ELEM_OXYGEN) - 10100.0 * v * 0.031999 / rt;
    double co2 = 5070.0 * v * 0.04401 / rt;
    double expect = PR_MIN(need / (0.333e-3 / 60.0), co2 / (0.368e-3 / 60.0)) / 200.0 * 60.0;
    int danger_at = -1;
    for (int t = 0; t < (int)(expect * 2.0); t++) {
        sim_tick(w, NULL);
        if (w->creatures[id].state == CREATURE_DANGER && danger_at < 0) {
            danger_at = t;
        }
        if (danger_at >= 0 && t > danger_at + 300) {
            break;
        }
    }
    EXPECT(danger_at > 0);
    EXPECT(fabs((double)danger_at - expect) < 0.25 * expect);
    EXPECT(w->creatures[id].health < 1.0f);
    world_destroy(w);
}

void test_creature_body_heat(void) {
    // In a sealed room of cold air (0 degC), the body stays near 37 degC and warms the air near it.
    World *w = air_room(128, 64, 77);
    for (int i = 0; i < w->width * w->height; i++) {
        if (g_elem_behavior[w->cells[i].element] == BEHAVIOR_GAS) {
            w->temp[i] = 273.15f;
        }
    }
    world_wake_heat(w, 0, 0, 127, 63);
    int id = creature_spawn(w, 60, 27);
    w->creatures[id].metabolism_scale = 0.0f;
    sim_run_ticks(w, 1800);
    Creature *c = &w->creatures[id];
    double body = 0.0, near = 0.0, far = 0.0;
    int nn = 0, nf = 0;
    for (int y = 1; y < 63; y++) {
        for (int x = 1; x < 127; x++) {
            size_t i = world_index(w, x, y);
            ElementId e = w->cells[i].element;
            if (e == ELEM_BODY) {
                body += (double)w->temp[i];
            } else if (g_elem_behavior[e] == BEHAVIOR_GAS) {
                bool is_near = x >= c->x - 4 && x < c->x + CREATURE_W + 4 && y >= c->y - 4;
                bool is_far = x < 20 || x > 107;
                if (is_near) {
                    near += (double)w->temp[i];
                    nn++;
                } else if (is_far) {
                    far += (double)w->temp[i];
                    nf++;
                }
            }
        }
    }
    body /= CREATURE_W * CREATURE_H;
    EXPECT(fabs(body - (double)CREATURE_BODY_K) < 2.0);
    EXPECT(nn > 0 && nf > 0 && near / nn > far / nf + 2.0);
    EXPECT(c->heat_w > 0.0f);
    world_destroy(w);
}

static World *creature_world(JobPool *pool) {
    World *w = air_room(256, 128, 78);
    world_set_job_pool(w, pool);
    fill(w, 150, 90, 254, 126, ELEM_WATER);
    for (int k = 0; k < 3; k++) {
        int id = creature_spawn(w, 20 + 40 * k, 60);
        w->creatures[id].wander = 1;
        w->creatures[id].metabolism_scale = 20.0f;
    }
    sim_run_ticks(w, 300);
    return w;
}

void test_creatures_same_for_any_thread_count(void) {
    JobPool *pool = jobs_create(4);
    World *a = creature_world(NULL);
    World *b = creature_world(pool);
    size_t n = (size_t)a->width * a->height;
    EXPECT(memcmp(a->cells, b->cells, n * sizeof(Cell)) == 0);
    EXPECT(memcmp(a->mass, b->mass, n * sizeof(float)) == 0);
    EXPECT(memcmp(a->creatures, b->creatures, (size_t)a->creature_count * sizeof(Creature)) == 0);
    world_destroy(a);
    world_destroy(b);
    jobs_destroy(pool);
}

// ---- Gas currents: moving matter stirs the air ----

// A sealed room (192 x h world) of horizontal stripes of oxygen and nitrogen, 8 cells thick.
static World *striped_room(uint64_t seed, int h) {
    World *w = test_world_box(192, h, seed);
    for (int y = 1; y <= h - 2; y++) {
        fill(w, 1, y, 190, y, ((y / 8) & 1) ? ELEM_OXYGEN : ELEM_NITROGEN);
    }
    return w;
}

// Cells in the rect not holding their stripe's gas: how stirred it is.
static int64_t out_of_stripe(const World *w, int x0, int y0, int x1, int y1) {
    int64_t n = 0;
    for (int y = y0; y <= y1; y++) {
        ElementId want = ((y / 8) & 1) ? ELEM_OXYGEN : ELEM_NITROGEN;
        for (int x = x0; x <= x1; x++) {
            ElementId e = world_elem(w, x, y);
            n += g_elem_behavior[e] == BEHAVIOR_GAS && e != want;
        }
    }
    return n;
}

void test_walking_stirs_air(void) {
    // A creature walking along the floor drags the air around: the stripes it passes through get
    // mixed much more than in the same room with nobody walking (stripes also mix a little at
    // their edges on their own). Gas mass is conserved, and once it stops, the air calms down
    // about as much as the undisturbed room.
    World *w = striped_room(79, 64), *still = striped_room(79, 64);
    int id = creature_spawn(w, 10, 27);
    int id2 = creature_spawn(still, 10, 27);
    w->creatures[id].metabolism_scale = still->creatures[id2].metabolism_scale = 0.0f;
    double o2 = test_mass(w, ELEM_OXYGEN), n2 = test_mass(w, ELEM_NITROGEN);
    w->creatures[id].walk = 1;
    sim_run_ticks(w, 240); // about 110 cells
    sim_run_ticks(still, 240);
    w->creatures[id].walk = 0;
    int64_t path = out_of_stripe(w, 20, 20, 120, 62);
    int64_t calm = out_of_stripe(still, 20, 20, 120, 62);
    EXPECT(path > calm * 3 / 2);
    EXPECT(fabs(test_mass(w, ELEM_OXYGEN) - o2) < 1e-4 * o2);
    EXPECT(fabs(test_mass(w, ELEM_NITROGEN) - n2) < 1e-4 * n2);
    sim_run_ticks(w, 600);
    sim_run_ticks(still, 600);
    SimStats st, st2;
    sim_tick(w, &st);
    sim_tick(still, &st2);
    EXPECT(st.cells_visited < st2.cells_visited * 2 + 2000);
    world_destroy(w);
    world_destroy(still);
}

void test_falling_sand_stirs_air(void) {
    // Sand falling through the striped room churns the column it falls through, compared with the
    // same room without sand. Gas mass is conserved.
    World *w = striped_room(80, 128), *still = striped_room(80, 128);
    fill(w, 120, 2, 135, 20, ELEM_SAND);
    fill(still, 120, 2, 135, 20, ELEM_ROCK); // the same cells taken, but nothing falls
    double o2 = test_mass(w, ELEM_OXYGEN);
    sim_run_ticks(w, 300);
    sim_run_ticks(still, 300);
    int64_t column = out_of_stripe(w, 110, 30, 145, 100);
    int64_t calm = out_of_stripe(still, 110, 30, 145, 100);
    EXPECT(column > calm * 3 / 2);
    EXPECT(fabs(test_mass(w, ELEM_OXYGEN) - o2) < 1e-4 * o2);
    world_destroy(w);
    world_destroy(still);
}
