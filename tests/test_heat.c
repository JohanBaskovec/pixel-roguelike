#include "test.h"

#include "sim/edit.h"
#include "sim/sim.h"

#include <math.h>

void test_heat_conserves_energy(void);
void test_heat_never_overshoots(void);
void test_iron_conducts_faster_than_rock(void);
void test_ice_melts_in_warm_water(void);
void test_phase_changes_need_latent_heat(void);
void test_hot_gas_rises(void);
void test_heater_makes_plume(void);
void test_pool_heated_from_below_convects(void);
void test_moved_water_carries_heat(void);

static void paint(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT, 0.0f};
        sim_apply_edit(w, &ed);
    }
}

static void set_temp(World *w, int x0, int y0, int x1, int y1, float t) {
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            w->temp[world_index(w, x, y)] = t;
        }
    }
    world_wake_rect(w, x0, y0, x1, y1);
    world_wake_heat(w, x0, y0, x1, y1);
}

// Total thermal energy, sum of C * T over all cells (no cell is mid phase change here). A gas
// cell's C is its mass times its specific heat.
static double energy(const World *w) {
    double e = 0.0;
    for (int i = 0; i < w->width * w->height; i++) {
        ElementId el = w->cells[i].element;
        double cap = el == ELEM_GAS ? (double)gas_heat_cap(&w->gas[i]) : (double)g_elem_cap[el];
        e += cap * (double)w->temp[i];
    }
    return e;
}

static float temp_at(const World *w, int x, int y) {
    return elem_temp(w->cells[world_index(w, x, y)].element, w->temp[world_index(w, x, y)]);
}

void test_heat_conserves_energy(void) {
    // Iron, rock, water and oxygen at different temperatures (none near a phase change), with
    // water and gas moving: conduction and movement must not create or destroy heat.
    World *w = test_world_box(128, 128, 21);
    paint(w, 10, 80, 50, 126, ELEM_IRON);
    set_temp(w, 10, 80, 50, 126, 400.0f);
    paint(w, 51, 100, 126, 126, ELEM_ROCK);
    set_temp(w, 51, 100, 126, 126, 250.0f);
    paint(w, 60, 40, 120, 70, ELEM_WATER);
    set_temp(w, 60, 40, 120, 70, 330.0f);
    paint(w, 5, 5, 60, 40, ELEM_OXYGEN);
    set_temp(w, 5, 5, 60, 40, 350.0f);
    double e0 = energy(w);
    sim_run_ticks(w, 600);
    double e1 = energy(w);
    EXPECT(fabs(e1 - e0) / e0 < 1e-4);
    // Heat did flow across the iron/rock border (x = 50/51).
    EXPECT(temp_at(w, 48, 110) < 399.0f);
    EXPECT(temp_at(w, 53, 110) > 251.0f);
    world_destroy(w);
}

void test_heat_never_overshoots(void) {
    // Extreme contrasts (very light hot gas against cold iron): explicit schemes oscillate here.
    // Temperatures must stay within the starting range.
    World *w = test_world_box(64, 64, 22);
    paint(w, 1, 32, 62, 62, ELEM_IRON);
    set_temp(w, 0, 0, 63, 63, 100.0f);
    paint(w, 1, 1, 62, 31, ELEM_HYDROGEN);
    set_temp(w, 1, 1, 62, 31, 3000.0f);
    float lo = 1e9f, hi = -1e9f;
    for (int t = 0; t < 300; t++) {
        sim_tick(w, NULL);
        for (int i = 0; i < 64 * 64; i++) {
            if (g_elem_cap[w->cells[i].element] > 0.0f) {
                lo = PR_MIN(lo, w->temp[i]);
                hi = PR_MAX(hi, w->temp[i]);
            }
        }
    }
    EXPECT(lo >= 100.0f - 1e-3f);
    EXPECT(hi <= 3000.0f + 1e-2f);
    // All that hydrogen holds ~0.9 MJ; the iron takes ~0.85 MJ per kelvin: it can only warm by
    // about 1 K even at equilibrium. Check that heat arrived at its surface.
    EXPECT(temp_at(w, 30, 32) > 100.05f);
    world_destroy(w);
}

void test_iron_conducts_faster_than_rock(void) {
    // Two bars in vacuum (which insulates), hot at their left end. Heat reaches the far end of
    // the iron bar first.
    World *w = test_world_box(128, 64, 23);
    paint(w, 10, 20, 70, 22, ELEM_IRON);
    paint(w, 10, 40, 70, 42, ELEM_ROCK);
    set_temp(w, 10, 20, 70, 22, 293.15f);
    set_temp(w, 10, 40, 70, 42, 293.15f);
    for (int t = 0; t < 900; t++) {
        set_temp(w, 10, 20, 12, 22, 1000.0f); // keep the left ends hot
        set_temp(w, 10, 40, 12, 42, 1000.0f);
        sim_tick(w, NULL);
    }
    // 10 cells from the hot end. Iron: about 0.1 s of game time per cell (with the speed-up), so
    // it is well warmed after 15 s; rock is ~17x slower.
    float iron_far = temp_at(w, 22, 21), rock_far = temp_at(w, 22, 41);
    EXPECT(iron_far > 400.0f);
    EXPECT(iron_far > rock_far + 50.0f);
    world_destroy(w);
}

void test_ice_melts_in_warm_water(void) {
    World *w = test_world_box(128, 128, 24);
    paint(w, 1, 40, 126, 126, ELEM_WATER);
    set_temp(w, 1, 40, 126, 126, 340.0f);
    paint(w, 54, 70, 74, 90, ELEM_ICE); // 21 x 21, starts at -10 degC
    int64_t ice0 = test_count(w, ELEM_ICE), water0 = test_count(w, ELEM_WATER);
    sim_run_ticks(w, 1500);
    int64_t ice1 = test_count(w, ELEM_ICE), water1 = test_count(w, ELEM_WATER);
    EXPECT(ice1 < ice0);
    EXPECT_EQ(ice0 + water0, ice1 + water1); // melting turns ice cells into water cells
    world_destroy(w);
}

void test_phase_changes_need_latent_heat(void) {
    // Single cells resting on the floor (y = 62), apart from each other.
    World *w = test_world_box(64, 64, 25);
    paint(w, 10, 62, 10, 62, ELEM_WATER);
    paint(w, 20, 62, 20, 62, ELEM_WATER);
    paint(w, 30, 62, 30, 62, ELEM_ICE);
    // Water at an (enthalpy) temperature just short of freezing: it stays water, at 0 degC.
    set_temp(w, 10, 62, 10, 62, 273.15f - 70.0f);
    // Water holding enough latent heat to boil, ice enough to melt. (Water vapour condenses at
    // its dew point: test_vapour_condenses_below_dew_point.)
    set_temp(w, 20, 62, 20, 62, 373.15f + 2257e3f / 4186.0f + 1.0f);
    set_temp(w, 30, 62, 30, 62, 273.15f + 334e3f / 2100.0f + 1.0f);
    sim_tick(w, NULL);
    EXPECT(test_cell_is(w, 10, 62, ELEM_WATER));
    EXPECT(fabsf(temp_at(w, 10, 62) - 273.15f) < 0.01f); // freezing: held at 0 degC
    bool heating = true;
    EXPECT(elem_phase_progress(ELEM_WATER, w->temp[world_index(w, 10, 62)], &heating) > 0.8f);
    EXPECT(!heating);
    // The others changed phase. (They may move once changed: look around their start.)
    EXPECT(test_count(w, ELEM_ICE) == 0);
    EXPECT(test_count(w, ELEM_STEAM) >= 1);
    EXPECT(test_count(w, ELEM_WATER) == 2); // 2 waters (one boiled) + melted ice
    world_destroy(w);
}

// Heat above `base` held by gas in the rect (inclusive): sum of m * cp * (T - base), J.
static double gas_heat_in(const World *w, int x0, int y0, int x1, int y1, float base) {
    double q = 0.0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            size_t i = world_index(w, x, y);
            ElementId e = w->cells[i].element;
            if (e == ELEM_GAS) {
                q += (double)gas_heat_cap(&w->gas[i]) *
                     (double)PR_MAX(elem_temp(e, w->temp[i]) - base, 0.0f);
            }
        }
    }
    return q;
}

// Mean height (y, +y down) of the heat above `base` held by gas, weighted by m * cp * (T - base).
static double hot_gas_height(const World *w, float base) {
    double sum = 0.0, wsum = 0.0;
    for (int y = 0; y < w->height; y++) {
        double q = gas_heat_in(w, 0, y, w->width - 1, y, base);
        sum += q * y;
        wsum += q;
    }
    return wsum > 0.0 ? sum / wsum : -1.0;
}

void test_hot_gas_rises(void) {
    // A room of oxygen with a patch near the floor suddenly heated to 700 K: it expands (pushing
    // the cold gas aside) and its heat rises. Slowly: expanding by trading mass with cooler
    // neighbours mixes part of its heat into them (docs/SIMULATION.md -> Gases, limitations).
    World *w = test_world_box(128, 128, 26);
    paint(w, 1, 1, 126, 126, ELEM_OXYGEN);
    set_temp(w, 1, 1, 126, 126, 293.15f);
    set_temp(w, 44, 100, 84, 120, 700.0f);
    double y0 = hot_gas_height(w, 293.15f);
    sim_run_ticks(w, 400);
    double y1 = hot_gas_height(w, 293.15f);
    EXPECT(y1 < y0 - 3.0); // the heat's mean height rose by more than 3 cells
    world_destroy(w);
}

void test_heater_makes_plume(void) {
    // A room of oxygen over a hot iron plate: the gas it heats rises, so more heat ends up above
    // the plate than beside it.
    World *w = test_world_box(128, 128, 28);
    paint(w, 1, 1, 126, 126, ELEM_OXYGEN);
    paint(w, 54, 122, 74, 126, ELEM_IRON);
    for (int t = 0; t < 600; t++) {
        set_temp(w, 54, 122, 74, 126, 700.0f); // a heater held at 700 K
        sim_tick(w, NULL);
    }
    double above = gas_heat_in(w, 50, 60, 78, 115, 293.15f);
    double beside =
        gas_heat_in(w, 1, 100, 45, 126, 293.15f) + gas_heat_in(w, 83, 100, 126, 126, 293.15f);
    EXPECT(above > 3.0 * beside);
    world_destroy(w);
}

void test_pool_heated_from_below_convects(void) {
    // A 100-cell-deep pool with warm water at the bottom. By conduction alone (even sped up) the
    // top would not notice for hours; convection must carry warm water up within seconds.
    World *w = test_world_box(64, 128, 27);
    paint(w, 1, 27, 62, 126, ELEM_WATER);
    set_temp(w, 1, 27, 62, 126, 290.0f);
    set_temp(w, 1, 117, 62, 126, 350.0f);
    sim_run_ticks(w, 1200);
    double top = 0.0;
    for (int x = 1; x < 63; x++) {
        top += (double)temp_at(w, x, 30);
    }
    top /= 62.0;
    EXPECT(top > 295.0);
    world_destroy(w);
}

void test_moved_water_carries_heat(void) {
    // A U-tube with hot water on the high side: water pressure moves water into the cold side,
    // and its heat goes with it.
    World *w = test_world_box(256, 192, 28);
    paint(w, 127, 1, 129, 160, ELEM_DIRT);
    paint(w, 1, 60, 126, 190, ELEM_WATER);
    set_temp(w, 1, 60, 126, 190, 340.0f);
    paint(w, 127, 161, 129, 190, ELEM_WATER);
    paint(w, 130, 150, 254, 190, ELEM_WATER);
    set_temp(w, 127, 150, 254, 190, 290.0f);
    sim_run_ticks(w, 1500);
    float hottest_right = 0.0f;
    for (int y = 60; y < 150; y++) {
        for (int x = 130; x < 255; x++) {
            if (test_cell_is(w, x, y, ELEM_WATER)) {
                hottest_right = PR_MAX(hottest_right, temp_at(w, x, y));
            }
        }
    }
    EXPECT(hottest_right > 320.0f);
    world_destroy(w);
}
