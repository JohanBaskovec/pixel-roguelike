#include "test.h"

#include "core/jobs.h"
#include "sim/edit.h"
#include "sim/sim.h"

#include <math.h>
#include <string.h>

#define ATM ((double)P_ATM)

void test_gas_fills_vacuum(void);
void test_gas_pressure_equalizes(void);
void test_sealed_gas_pressure_follows_temperature(void);
void test_boiling_keeps_mass(void);
void test_condensing_keeps_mass_on_average(void);
void test_air_stays_mixed(void);
void test_gases_mix_and_settle(void);
void test_gas_mixture_stays_mixed(void);
void test_co2_blob_sinks_and_pools(void);
void test_dilute_co2_stays_in_air(void);
void test_bulk_buoyancy_same_for_any_thread_count(void);
void test_pressurised_tank_drives_water_up_a_pipe(void);
void test_air_holds_water_up_a_vacuum_tube(void);
void test_trapped_air_is_compressed(void);

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

// A sealed room (1..126 x 1..126) of air, with CO2 where `co2(x, y, h)` says so (h: a hash).
static World *co2_room(uint64_t seed, bool (*co2)(int x, int y, uint32_t h)) {
    World *w = test_world_box(128, 128, seed);
    for (int y = 1; y <= 126; y++) {
        for (int x = 1; x <= 126; x++) {
            uint32_t h = (uint32_t)(x * 7919 + y * 104729) * 2654435761u;
            ElementId e = (h >> 16) % 100 < 21 ? ELEM_OXYGEN : ELEM_NITROGEN;
            world_set_cell(w, x, y, co2(x, y, h) ? ELEM_CO2 : e, h);
        }
    }
    world_wake_rect(w, 0, 0, 127, 127);
    return w;
}

static bool co2_half(int x, int y, uint32_t h) {
    (void)x;
    (void)y;
    return (h >> 8) & 1;
}

static bool co2_blob(int x, int y, uint32_t h) {
    (void)h;
    return (x - 64) * (x - 64) + (y - 30) * (y - 30) < 12 * 12;
}

static bool co2_dilute(int x, int y, uint32_t h) {
    (void)x;
    (void)y;
    return (h >> 8) % 100 < 5;
}

void test_gas_mixture_stays_mixed(void) {
    // Half CO2, half air, mixed cell by cell: a mixture is a gas of its own (denser than air, but
    // the same everywhere), so it does not unmix, and it goes to sleep.
    World *w = co2_room(48, co2_half);
    double y0 = test_mean_y(w, ELEM_CO2);
    sim_run_ticks(w, 3000);
    EXPECT(fabs(test_mean_y(w, ELEM_CO2) - y0) < 3.0);
    SimStats st;
    sim_tick(w, &st);
    EXPECT(st.cells_visited < 2000);
    world_destroy(w);
}

void test_co2_blob_sinks_and_pools(void) {
    // A ball of CO2 (1.2 m across) high in a room of air sinks as a whole (bulk buoyancy), spreads
    // over the floor and pools there; little of it mixes into the air on the way. Mass is kept.
    World *w = co2_room(49, co2_blob);
    double m = test_mass(w, ELEM_CO2);
    sim_run_ticks(w, 1800);
    EXPECT(fabs(test_mass(w, ELEM_CO2) - m) < 1e-4 * m);
    int64_t low = 0, all = test_count(w, ELEM_CO2), spread = 0;
    for (int y = 106; y <= 126; y++) {
        for (int x = 1; x <= 126; x++) {
            low += test_cell_is(w, x, y, ELEM_CO2);
        }
    }
    for (int x = 1; x <= 126; x++) {
        spread += test_cell_is(w, x, 126, ELEM_CO2);
    }
    EXPECT(low > all / 2); // most of it is in the bottom 20 rows
    EXPECT(spread > 40);   // spread over the floor, wider than the ball was
    EXPECT(test_mean_y(w, ELEM_CO2) > 90.0);
    world_destroy(w);
}

void test_dilute_co2_stays_in_air(void) {
    // Air with 5% CO2 (as breathed out into a room): the CO2 stays mixed in, it does not rain out
    // onto the floor cell by cell.
    World *w = co2_room(50, co2_dilute);
    double y0 = test_mean_y(w, ELEM_CO2);
    sim_run_ticks(w, 3000);
    EXPECT(fabs(test_mean_y(w, ELEM_CO2) - y0) < 3.0);
    world_destroy(w);
}

void test_bulk_buoyancy_same_for_any_thread_count(void) {
    // The ball of CO2 sinks through the boundary between chunks (lifts reach 31 cells, and the
    // bulk windows 16): bit-identical on 1 and 4 threads.
    JobPool *pool = jobs_create(4);
    World *a = co2_room(51, co2_blob), *b = co2_room(51, co2_blob);
    world_set_job_pool(b, pool);
    sim_run_ticks(a, 300);
    sim_run_ticks(b, 300);
    size_t n = (size_t)a->width * a->height;
    EXPECT(memcmp(a->cells, b->cells, n * sizeof(Cell)) == 0);
    EXPECT(memcmp(a->mass, b->mass, n * sizeof(float)) == 0);
    EXPECT(memcmp(a->temp, b->temp, n * sizeof(float)) == 0);
    EXPECT(test_mean_y(a, ELEM_CO2) > 64.0); // it did cross into the lower chunks
    world_destroy(a);
    world_destroy(b);
    jobs_destroy(pool);
}

// ---- Gas pressure on liquids ----

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            world_set_cell(w, x, y, e, (uint32_t)(x * 7919 + y * 104729));
        }
    }
    world_wake_rect(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
    world_wake_heat(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
}

// Fills the rect with air (each cell oxygen or nitrogen) at `atm` atmospheres.
static void fill_air(World *w, int x0, int y0, int x1, int y1, float atm) {
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            uint32_t h = (uint32_t)(x * 7919 + y * 104729) * 2654435761u;
            ElementId e = (h >> 16) % 100 < 21 ? ELEM_OXYGEN : ELEM_NITROGEN;
            world_set_cell(w, x, y, e, h);
            w->mass[world_index(w, x, y)] *= atm;
        }
    }
    world_wake_rect(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
    world_wake_heat(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
}

// Highest row (smallest y) holding water in column x between rows y0 and y1, or -1.
static int water_top(const World *w, int x, int y0, int y1) {
    for (int y = y0; y <= y1; y++) {
        if (test_cell_is(w, x, y, ELEM_WATER)) {
            return y;
        }
    }
    return -1;
}

// Mean gas pressure (Pa) in the rect.
static double mean_pressure(const World *w, int x0, int y0, int x1, int y1) {
    double lo, hi, mean;
    pressure_range(w, x0, y0, x1, y1, &lo, &hi, &mean);
    return mean;
}

static double water_head_cells(double pa) {
    return pa / ((double)g_elem_defs[ELEM_WATER].density * (double)GRAVITY * (double)CELL_SIZE_M);
}

void test_pressurised_tank_drives_water_up_a_pipe(void) {
    // A sealed tank: water below, nitrogen at 0.3 atm above. A pipe leaves its bottom and rises
    // into vacuum. The gas pushes water up the pipe until the column's weight matches the gas
    // pressure (which drops a little as the gas expands into the room the water left).
    World *w = test_world_box(128, 256, 48);
    fill(w, 1, 1, 126, 254, ELEM_DIRT);
    fill(w, 10, 200, 60, 250, ELEM_VOID);  // tank
    fill(w, 61, 246, 72, 250, ELEM_VOID);  // channel from its bottom
    fill(w, 70, 20, 72, 245, ELEM_VOID);   // pipe up
    fill(w, 40, 5, 120, 19, ELEM_VOID);    // room at the top (vacuum)
    fill(w, 10, 225, 60, 250, ELEM_WATER); // 26 rows of water
    for (int y = 200; y <= 224; y++) {
        for (int x = 10; x <= 60; x++) {
            world_set_gas(w, x, y, ELEM_NITROGEN, 0.3f * gas_mass_at(ELEM_NITROGEN, P_ATM, T_ROOM),
                          (uint32_t)(x + y));
        }
    }
    world_wake_rect(w, 0, 0, 127, 255);
    double n2 = test_mass(w, ELEM_NITROGEN);
    sim_run_ticks(w, 2400);
    int tank = water_top(w, 35, 200, 250), pipe = water_top(w, 71, 20, 250);
    double head = water_head_cells(mean_pressure(w, 10, 200, 60, 250));
    EXPECT(tank > 0 && pipe > 0);
    EXPECT(tank - pipe > 30);                 // water stands well above the tank's level
    EXPECT(fabs((tank - pipe) - head) < 3.0); // ... by the gas pressure, as a water height
    EXPECT(fabs(test_mass(w, ELEM_NITROGEN) - n2) < 1e-4 * n2);
    world_destroy(w);
}

void test_air_holds_water_up_a_vacuum_tube(void) {
    // A barometer: a tube sealed at the top, empty, its open bottom under the surface of a pool in
    // a room of air at 1 atm. Air pushes water up the tube until the column weighs as much as the
    // air's pressure: about 207 cells (10.3 m) above the pool at 1 atm (the air expands a little
    // into the room the pool's water left).
    World *w = test_world_box(128, 320, 49);
    fill(w, 1, 1, 126, 318, ELEM_DIRT);
    fill(w, 1, 150, 126, 318, ELEM_VOID);  // room
    fill(w, 1, 290, 126, 318, ELEM_WATER); // pool
    fill(w, 60, 20, 64, 305, ELEM_DIRT);   // tube walls, down into the pool
    fill(w, 61, 21, 63, 305, ELEM_VOID);   // tube interior: vacuum (water at the bottom below)
    fill(w, 61, 290, 63, 305, ELEM_WATER);
    fill_air(w, 1, 150, 59, 289, 1.0f);
    fill_air(w, 65, 150, 126, 289, 1.0f);
    sim_run_ticks(w, 2400);
    int pool = water_top(w, 20, 150, 318), tube = water_top(w, 62, 21, 318);
    double head = water_head_cells(mean_pressure(w, 1, 150, 59, 289));
    EXPECT(pool > 0 && tube > 0);
    EXPECT(head > 190.0 && head < 210.0); // 1 atm, a little less as the air expands
    EXPECT(fabs((pool - tube) - head) < 4.0);
    world_destroy(w);
}

void test_trapped_air_is_compressed(void) {
    // A sealed room of thin air (0.1 atm) fed from below by a tall column of water open to vacuum.
    // Water enters and squeezes the air (its mass stays, its pressure rises) until the air's
    // pressure balances the column.
    World *w = test_world_box(128, 192, 50);
    fill(w, 1, 1, 126, 190, ELEM_DIRT);
    fill(w, 20, 140, 80, 180, ELEM_VOID); // room
    fill(w, 81, 176, 92, 180, ELEM_VOID); // channel from its bottom
    fill(w, 90, 20, 92, 175, ELEM_VOID);  // column
    fill(w, 90, 40, 92, 175, ELEM_WATER);
    fill(w, 81, 176, 92, 180, ELEM_WATER);
    fill(w, 20, 168, 80, 180, ELEM_WATER); // over the channel, so no air gets into it
    fill_air(w, 20, 140, 80, 167, 0.1f);
    double air0 = test_mass(w, ELEM_OXYGEN) + test_mass(w, ELEM_NITROGEN);
    double p0 = mean_pressure(w, 20, 140, 80, 167);
    sim_run_ticks(w, 3000);
    int room = water_top(w, 50, 140, 180), column = water_top(w, 91, 20, 180);
    double p1 = mean_pressure(w, 20, 140, 80, 180);
    double air1 = test_mass(w, ELEM_OXYGEN) + test_mass(w, ELEM_NITROGEN);
    EXPECT(room > 0 && room < 168); // water came in
    EXPECT(p1 > 1.1 * p0);          // the air was compressed
    EXPECT(fabs(air1 - air0) < 1e-4 * air0);
    EXPECT(fabs((room - column) - water_head_cells(p1)) < 3.0);
    world_destroy(w);
}
