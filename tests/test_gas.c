#include "test.h"

#include "sim/edit.h"
#include "sim/sim.h"

#include <math.h>

#define ATM ((double)P_ATM)

void test_gas_fills_vacuum(void);
void test_gas_pressure_equalizes(void);
void test_sealed_gas_pressure_follows_temperature(void);
void test_boiling_keeps_mass(void);
void test_condensing_keeps_mass_on_average(void);
void test_air_stays_mixed(void);
void test_gases_mix_and_settle(void);

static void paint(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT};
        sim_apply_edit(w, &ed);
    }
}

// Min, max and mean pressure (Pa) of the gas cells in the rect.
static void pressure_range(const World *w, int x0, int y0, int x1, int y1, double *lo, double *hi,
                           double *mean) {
    *lo = 1e30;
    *hi = 0.0;
    double sum = 0.0;
    int n = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            double p = (double)world_gas_pressure(w, x, y);
            if (p > 0.0) {
                *lo = fmin(*lo, p);
                *hi = fmax(*hi, p);
                sum += p;
                n++;
            }
        }
    }
    *mean = n ? sum / n : 0.0;
}

void test_gas_fills_vacuum(void) {
    // A blob of oxygen at 1 atm in a sealed box of vacuum ten times its size: it fills the box,
    // thinning to about 0.1 atm everywhere, and keeps its mass.
    World *w = test_world_box(64, 64, 41);
    paint(w, 26, 26, 37, 37, ELEM_OXYGEN); // 144 cells; the box holds 62 * 62 = 3844
    double m0 = test_mass(w, ELEM_OXYGEN);
    sim_run_ticks(w, 1500);
    EXPECT(fabs(test_mass(w, ELEM_OXYGEN) - m0) <= 1e-4 * m0);
    EXPECT_EQ(test_count(w, ELEM_VOID), 0); // no vacuum left inside (walls are dirt)
    double lo, hi, mean;
    pressure_range(w, 1, 1, 62, 62, &lo, &hi, &mean);
    double expect = ATM * 144.0 / 3844.0;
    EXPECT(fabs(mean - expect) < 0.02 * expect);
    EXPECT(hi < 1.2 * lo); // nearly uniform
    world_destroy(w);
}

void test_gas_pressure_equalizes(void) {
    // Two rooms of nitrogen, at 2 atm and 1 atm, joined by a gap in the wall: they end at
    // 1.5 atm.
    World *w = test_world_box(128, 64, 42);
    paint(w, 1, 1, 126, 62, ELEM_NITROGEN);
    for (int y = 1; y <= 62; y++) {
        for (int x = 1; x < 64; x++) {
            w->mass[world_index(w, x, y)] *= 2.0f;
        }
    }
    paint(w, 64, 1, 64, 50, ELEM_DIRT); // wall with a 12-cell gap at the bottom
    sim_run_ticks(w, 3000);
    double lo, hi, mean;
    pressure_range(w, 1, 1, 126, 62, &lo, &hi, &mean);
    EXPECT(fabs(mean - 1.5 * ATM) < 0.03 * ATM);
    EXPECT(hi - lo < 0.03 * ATM);
    world_destroy(w);
}

void test_sealed_gas_pressure_follows_temperature(void) {
    // Heating a sealed room of nitrogen from 293 K to 586 K doubles its pressure (P ∝ T).
    World *w = test_world_box(64, 64, 43);
    paint(w, 1, 1, 62, 62, ELEM_NITROGEN);
    double lo, hi, p0, p1;
    pressure_range(w, 1, 1, 62, 62, &lo, &hi, &p0);
    for (int y = 0; y < 64; y++) { // the walls too, or they would cool the gas
        for (int x = 0; x < 64; x++) {
            w->temp[world_index(w, x, y)] = 2.0f * T_ROOM;
        }
    }
    world_wake_rect(w, 0, 0, 63, 63);
    world_wake_heat(w, 0, 0, 63, 63);
    sim_run_ticks(w, 300);
    pressure_range(w, 1, 1, 62, 62, &lo, &hi, &p1);
    EXPECT(fabs(p1 / p0 - 2.0) < 0.01);
    world_destroy(w);
}

void test_boiling_keeps_mass(void) {
    // A pool of boiling water in a room of air: each boiled cell becomes steam with the water's
    // mass (at very high pressure), which pushes the air aside and expands. Steam mass equals
    // the mass of water lost.
    World *w = test_world_box(128, 128, 44);
    Edit air = {1, 1, 126, 1, 0, EDIT_SQUARE, EDIT_AIR, 0, EDIT_PAINT};
    for (int y = 1; y <= 126; y++) {
        air.y0 = air.y1 = y;
        sim_apply_edit(w, &air);
    }
    paint(w, 40, 120, 88, 126, ELEM_WATER);
    int64_t water0 = test_count(w, ELEM_WATER);
    double air0 = test_mass(w, ELEM_OXYGEN) + test_mass(w, ELEM_NITROGEN);
    for (int t = 0; t < 120; t++) {
        for (int x = 40; x <= 88; x++) { // a hot floor under the pool
            w->temp[world_index(w, x, 126)] =
                PR_MAX(w->temp[world_index(w, x, 126)], 373.15f + 2257e3f / 4186.0f + 5.0f);
        }
        world_wake_heat(w, 40, 126, 88, 126);
        sim_tick(w, NULL);
    }
    int64_t boiled = water0 - test_count(w, ELEM_WATER);
    EXPECT(boiled > 0);
    double steam = test_mass(w, ELEM_STEAM);
    double expect = (double)boiled * (double)g_elem_mass[ELEM_WATER];
    EXPECT(fabs(steam - expect) < 1e-3 * expect);
    double air1 = test_mass(w, ELEM_OXYGEN) + test_mass(w, ELEM_NITROGEN);
    EXPECT(fabs(air1 - air0) < 1e-4 * air0);
    world_destroy(w);
}

void test_condensing_keeps_mass_on_average(void) {
    // Cells of steam holding 40% of a water cell's mass, cooled far below 100 °C: each becomes a
    // full water cell with probability 0.4 (else vanishes), so the water made matches the steam's
    // mass on average.
    World *w = test_world_box(128, 128, 45);
    float m = 0.4f * g_elem_mass[ELEM_WATER];
    int n = 0;
    for (int y = 10; y < 120; y += 2) {
        for (int x = 10; x < 120; x += 2) {
            world_set_gas(w, x, y, ELEM_STEAM, m, (uint32_t)(x * 31 + y));
            w->temp[world_index(w, x, y)] = 373.15f - 2257e3f / 2080.0f - 1.0f;
            n++;
        }
    }
    world_wake_rect(w, 0, 0, 127, 127);
    world_wake_heat(w, 0, 0, 127, 127);
    sim_tick(w, NULL);
    EXPECT_EQ(test_count(w, ELEM_STEAM), 0);
    double expect = 0.4 * n; // 1210 of 3025
    double sd = sqrt(n * 0.4 * 0.6);
    EXPECT(fabs((double)test_count(w, ELEM_WATER) - expect) < 4.0 * sd);
    world_destroy(w);
}

void test_air_stays_mixed(void) {
    // Air (21% oxygen, 79% nitrogen cells) does not unmix: oxygen does not pool at the floor.
    // And a room of still air goes to sleep.
    World *w = test_world_box(64, 128, 46);
    Edit air = {1, 1, 62, 1, 0, EDIT_SQUARE, EDIT_AIR, 0, EDIT_PAINT};
    for (int y = 1; y <= 126; y++) {
        air.y0 = air.y1 = y;
        sim_apply_edit(w, &air);
    }
    double frac = (double)test_count(w, ELEM_OXYGEN) / (62.0 * 126.0);
    EXPECT(fabs(frac - (double)AIR_O2_FRAC) < 0.02);
    sim_run_ticks(w, 2000);
    EXPECT(fabs(test_mean_y(w, ELEM_OXYGEN) - test_mean_y(w, ELEM_NITROGEN)) < 3.0);
    SimStats st;
    sim_tick(w, &st);
    EXPECT(st.cells_visited < 200); // mixed and at one pressure: asleep
    world_destroy(w);
}

void test_gases_mix_and_settle(void) {
    // Oxygen on the left, nitrogen on the right (same buoyancy): they spread into each other at
    // the interface, then stop moving.
    World *w = test_world_box(128, 64, 47);
    paint(w, 1, 1, 63, 62, ELEM_OXYGEN);
    paint(w, 64, 1, 126, 62, ELEM_NITROGEN);
    sim_run_ticks(w, 3000);
    int64_t o2_right = 0;
    for (int y = 1; y <= 62; y++) {
        for (int x = 64; x <= 126; x++) {
            o2_right += test_cell_is(w, x, y, ELEM_OXYGEN);
        }
    }
    // Molecular diffusion is slow (about sqrt(2 D t) = 4.5 cm, one cell, in 50 s): a thin mixed
    // layer forms at the interface.
    EXPECT(o2_right > 20);
    SimStats st;
    for (int t = 0; t < 2000; t++) {
        sim_tick(w, &st);
    }
    EXPECT(st.cells_visited < 2000);
    world_destroy(w);
}
