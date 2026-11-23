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
void test_vapour_condenses_below_dew_point(void);
void test_air_stays_mixed(void);
void test_gases_diffuse_into_each_other(void);
void test_gas_mixture_stays_mixed(void);
void test_co2_blob_sinks_and_pools(void);
void test_warm_air_rises(void);
void test_gas_same_for_any_thread_count(void);
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
            gas_mix_scale(&w->gas[world_index(w, x, y)], 2.0f);
        }
    }
    paint(w, 64, 1, 64, 50, ELEM_DIRT); // wall with a 12-cell gap at the bottom
    sim_run_ticks(w, 3000);
    double lo, hi, mean;
    pressure_range(w, 1, 1, 126, 62, &lo, &hi, &mean);
    EXPECT(fabs(mean - 1.5 * ATM) < 0.03 * ATM);
    EXPECT(hi - lo < 0.035 * ATM); // evens out to GAS_EQ_TOL_FAR per 16 cells of path
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
    // A pool of boiling water in a room of hot air (390 K: the vapour does not condense): each
    // boiled cell becomes water vapour with the water's mass (at very high pressure), which
    // pushes the air aside and expands. The vapour's mass equals the mass of water lost.
    World *w = test_world_box(128, 128, 44);
    Edit air = {1, 1, 126, 1, 0, EDIT_SQUARE, EDIT_AIR, 0, EDIT_PAINT};
    for (int y = 1; y <= 126; y++) {
        air.y0 = air.y1 = y;
        sim_apply_edit(w, &air);
    }
    for (int i = 0; i < w->width * w->height; i++) {
        if (w->cells[i].element == ELEM_GAS) {
            w->temp[i] = 390.0f;
        }
    }
    paint(w, 40, 120, 88, 126, ELEM_WATER);
    int64_t water0 = test_count(w, ELEM_WATER);
    double air0 = test_mass(w, ELEM_GAS) - test_mass(w, ELEM_STEAM);
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
    double air1 = test_mass(w, ELEM_GAS) - test_mass(w, ELEM_STEAM);
    EXPECT(fabs(air1 - air0) < 1e-4 * air0);
    world_destroy(w);
}

void test_vapour_condenses_below_dew_point(void) {
    // Humid air (air with 60% water vapour by moles: its dew point is 86 degC) at 90 degC stays
    // vapour. Held at 20 degC (as a cooling brush would; condensing releases latent heat), the
    // vapour condenses until the air is saturated (2.3% at 20 degC), into droplets of water that
    // rain onto the floor; the droplets hold the vapour lost, on average (each is a full water
    // cell).
    World *w = test_world_box(128, 128, 45);
    const float t0 = 363.15f;
    for (int y = 1; y <= 126; y++) {
        for (int x = 1; x <= 126; x++) {
            GasMix g = gas_mix_air(0.4f * P_ATM, t0);
            g.m[GAS_H2O] = 0.6f * P_ATM * CELL_VOLUME_M3 / (GAS_R * t0) * g_species_molar[GAS_H2O];
            world_set_mix(w, x, y, &g, t0, (uint32_t)(x * 31 + y));
        }
    }
    for (int i = 0; i < w->width * w->height; i++) {
        w->temp[i] = t0;
    }
    world_wake_rect(w, 0, 0, 127, 127);
    world_wake_heat(w, 0, 0, 127, 127);
    double v0 = test_mass(w, ELEM_STEAM);
    sim_run_ticks(w, 60);
    EXPECT_EQ(test_count(w, ELEM_WATER), 0);
    EXPECT(fabs(test_mass(w, ELEM_STEAM) - v0) < 1e-4 * v0);
    for (int t = 0; t < 600; t++) {
        for (int i = 0; i < w->width * w->height; i++) {
            if (w->cells[i].element == ELEM_GAS) {
                w->temp[i] = 293.15f;
            }
        }
        world_wake_heat(w, 0, 0, 127, 127);
        sim_tick(w, NULL);
    }
    double v1 = test_mass(w, ELEM_STEAM);
    // Saturated: the vapour's share of the gas's moles is about p_sat / p.
    double n_gas = 0.0, n_v = 0.0;
    for (int i = 0; i < w->width * w->height; i++) {
        if (w->cells[i].element == ELEM_GAS) {
            n_gas += (double)gas_moles(&w->gas[i]);
            n_v += (double)(w->gas[i].m[GAS_H2O] * g_species_inv_molar[GAS_H2O]);
        }
    }
    double x_sat = (double)water_saturation_pressure(293.15f) / ATM;
    EXPECT(n_v / n_gas < 1.5 * x_sat);
    int64_t drops = test_count(w, ELEM_WATER);
    double expect = (v0 - v1) / (double)g_elem_mass[ELEM_WATER];
    EXPECT(drops > 0);
    EXPECT(fabs((double)drops - expect) < 4.0 * sqrt(expect) + 1.0);
    world_destroy(w);
}

void test_air_stays_mixed(void) {
    // Painted air is the same mixture in every cell (20.95% oxygen, 0.04% CO2, the rest
    // nitrogen): it does not unmix, and a room of still air goes to sleep at once.
    World *w = test_world_box(64, 128, 46);
    Edit air = {1, 1, 62, 1, 0, EDIT_SQUARE, EDIT_AIR, 0, EDIT_PAINT};
    for (int y = 1; y <= 126; y++) {
        air.y0 = air.y1 = y;
        sim_apply_edit(w, &air);
    }
    const GasMix *g = &w->gas[world_index(w, 30, 60)];
    double x_o2 = (double)(g->m[GAS_O2] * g_species_inv_molar[GAS_O2] / gas_moles(g));
    EXPECT(fabs(x_o2 - (double)AIR_O2_FRAC) < 1e-4);
    double y_o2 = test_mean_y(w, ELEM_OXYGEN), y_n2 = test_mean_y(w, ELEM_NITROGEN);
    sim_run_ticks(w, 300);
    EXPECT(fabs(test_mean_y(w, ELEM_OXYGEN) - y_o2) < 0.01);
    EXPECT(fabs(test_mean_y(w, ELEM_NITROGEN) - y_n2) < 0.01);
    SimStats st;
    sim_tick(w, &st);
    EXPECT(st.cells_visited < 200); // mixed and at one pressure: asleep
    world_destroy(w);
}

void test_gases_diffuse_into_each_other(void) {
    // Nitrogen above, oxygen below (denser: a stable layering, so only diffusion acts): they
    // diffuse into each other, the oxygen that crossed growing as the square root of time
    // (Fick), at the game's diffusivity (about 2.5e-3 m^2/s, gentle indoor turbulence):
    // sqrt(D t / pi) = 3 cells' worth of oxygen per column above the boundary after 30 s, twice
    // that after 2 min.
    World *w = test_world_box(64, 128, 47);
    paint(w, 1, 1, 62, 63, ELEM_NITROGEN);
    paint(w, 1, 64, 62, 126, ELEM_OXYGEN);
    float cell = gas_mass_at(ELEM_OXYGEN, P_ATM, T_ROOM);
    double crossed[2];
    for (int k = 0; k < 2; k++) {
        sim_run_ticks(w, k == 0 ? 1800 : 5400);
        double m = 0.0;
        for (int y = 1; y <= 63; y++) {
            for (int x = 1; x <= 62; x++) {
                m += (double)w->gas[world_index(w, x, y)].m[GAS_O2];
            }
        }
        crossed[k] = m / (double)cell / 62.0; // cells' worth per column
    }
    EXPECT(crossed[0] > 2.0 && crossed[0] < 5.0);
    double ratio = crossed[1] / crossed[0];
    EXPECT(ratio > 1.6 && ratio < 2.4); // sqrt(4) = 2
    world_destroy(w);
}

// A sealed room (1..126 x 1..126) of air, with CO2 where `co2(x, y)` says so.
static World *co2_room(uint64_t seed, bool (*co2)(int x, int y)) {
    World *w = test_world_box(128, 128, seed);
    for (int y = 1; y <= 126; y++) {
        for (int x = 1; x <= 126; x++) {
            uint32_t h = (uint32_t)(x * 7919 + y * 104729) * 2654435761u;
            world_set_cell(w, x, y, co2(x, y) ? ELEM_CO2 : ELEM_GAS, h);
        }
    }
    world_wake_rect(w, 0, 0, 127, 127);
    return w;
}

static bool co2_blob(int x, int y) {
    return (x - 64) * (x - 64) + (y - 30) * (y - 30) < 12 * 12;
}

void test_gas_mixture_stays_mixed(void) {
    // An even mixture (half CO2, half air, the same in every cell) is a gas of its own, denser
    // than air but the same everywhere: it does not unmix, and it sleeps. So does air with 5% CO2
    // (as breathed out into a room).
    for (int k = 0; k < 2; k++) {
        World *w = test_world_box(128, 128, 48 + (uint64_t)k);
        float share = k == 0 ? 0.5f : 0.05f;
        for (int y = 1; y <= 126; y++) {
            for (int x = 1; x <= 126; x++) {
                GasMix g = gas_mix_air((1.0f - share) * P_ATM, T_ROOM);
                g.m[GAS_CO2] += gas_mass_at(ELEM_CO2, share * P_ATM, T_ROOM);
                world_set_mix(w, x, y, &g, T_ROOM, (uint32_t)(x + y));
            }
        }
        world_wake_rect(w, 0, 0, 127, 127);
        double y0 = test_mean_y(w, ELEM_CO2);
        sim_run_ticks(w, 300);
        EXPECT(fabs(test_mean_y(w, ELEM_CO2) - y0) < 0.01);
        SimStats st;
        sim_tick(w, &st);
        EXPECT(st.cells_visited < 200);
        world_destroy(w);
    }
}

// Highest CO2 mole fraction in any gas cell with y in [y0, y1).
static double max_co2_fraction(const World *w, int y0, int y1) {
    double best = 0.0;
    for (int y = y0; y < y1; y++) {
        for (int x = 1; x < w->width - 1; x++) {
            size_t i = world_index(w, x, y);
            if (w->cells[i].element == ELEM_GAS) {
                const GasMix *g = &w->gas[i];
                best = fmax(best,
                            (double)(g->m[GAS_CO2] * g_species_inv_molar[GAS_CO2] / gas_moles(g)));
            }
        }
    }
    return best;
}

void test_co2_blob_sinks_and_pools(void) {
    // A ball of CO2 (1.2 m across) high in a room of air sinks (it is 50% denser), mixing with the
    // air on the way, and pools on the floor within seconds. There it spreads over the whole floor
    // (a gravity current) and keeps diffusing into the air: no pure CO2 is left, the pool thins,
    // and it stays a layer of CO2-rich air under clean air. Mass is kept.
    World *w = co2_room(49, co2_blob);
    double m = test_mass(w, ELEM_CO2);
    double y0 = test_mean_y(w, ELEM_CO2);
    sim_run_ticks(w, 600);
    EXPECT(test_mean_y(w, ELEM_CO2) > y0 + 40.0); // it sank
    EXPECT(max_co2_fraction(w, 1, 127) < 0.9);    // and mixed
    double peak = max_co2_fraction(w, 1, 127);
    sim_run_ticks(w, 1800);
    EXPECT(fabs(test_mass(w, ELEM_CO2) - m) < 1e-4 * m);
    EXPECT(test_mean_y(w, ELEM_CO2) > 70.0);          // a layer over the floor (63.5: evenly mixed)
    EXPECT(max_co2_fraction(w, 1, 127) < 0.8 * peak); // the pool thins
    EXPECT(max_co2_fraction(w, 1, 20) < 0.03);        // nearly clean air above it
    int columns = 0; // columns with at least 2% CO2 at the floor: it spread over it
    for (int x = 1; x <= 126; x++) {
        const GasMix *g = &w->gas[world_index(w, x, 125)];
        columns += g->m[GAS_CO2] * g_species_inv_molar[GAS_CO2] > 0.02f * gas_moles(g);
    }
    EXPECT(columns > 80);
    world_destroy(w);
}

void test_warm_air_rises(void) {
    // A pocket of air 30 K warmer than the room, near the floor, rises (it is 10% lighter): its
    // heat is at least 15 rows (75 cm) higher after 4 s.
    World *w = test_world_box(64, 128, 50);
    paint(w, 1, 1, 62, 126, EDIT_AIR);
    for (int y = 110; y <= 120; y++) {
        for (int x = 26; x <= 36; x++) {
            w->temp[world_index(w, x, y)] = T_ROOM + 30.0f;
        }
    }
    world_wake_rect(w, 0, 0, 63, 127);
    world_wake_heat(w, 0, 0, 63, 127);
    sim_run_ticks(w, 240);
    double sum = 0.0, wsum = 0.0; // mean height of the excess heat
    for (int y = 1; y <= 126; y++) {
        for (int x = 1; x <= 62; x++) {
            double d = (double)w->temp[world_index(w, x, y)] - (double)T_ROOM;
            if (d > 0.5) {
                sum += d * y;
                wsum += d;
            }
        }
    }
    EXPECT(wsum > 0.0 && sum / wsum < 100.0); // it started at 115
    world_destroy(w);
}

void test_gas_same_for_any_thread_count(void) {
    // The ball of CO2 sinks through the boundary between chunks: bit-identical on 1 and 4
    // threads.
    JobPool *pool = jobs_create(4);
    World *a = co2_room(51, co2_blob), *b = co2_room(51, co2_blob);
    world_set_job_pool(b, pool);
    sim_run_ticks(a, 300);
    sim_run_ticks(b, 300);
    size_t n = (size_t)a->width * a->height;
    EXPECT(memcmp(a->cells, b->cells, n * sizeof(Cell)) == 0);
    EXPECT(memcmp(a->gas, b->gas, n * sizeof(GasMix)) == 0);
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

// Fills the rect with air at `atm` atmospheres.
static void fill_air(World *w, int x0, int y0, int x1, int y1, float atm) {
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            uint32_t h = (uint32_t)(x * 7919 + y * 104729) * 2654435761u;
            world_set_cell(w, x, y, ELEM_GAS, h);
            gas_mix_scale(&w->gas[world_index(w, x, y)], atm);
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
    // a room of air at half an atmosphere. Air pushes water up the tube until the column weighs as
    // much as the air's pressure: about 103 cells (5.2 m) above the pool (a little less, as the
    // air expands into the room the pool's water left). (At 1 atm, 10.3 m: twice the world.)
    World *w = test_world_box(128, 192, 49);
    fill(w, 1, 1, 126, 190, ELEM_DIRT);
    fill(w, 1, 80, 126, 190, ELEM_VOID);   // room
    fill(w, 1, 162, 126, 190, ELEM_WATER); // pool
    fill(w, 60, 10, 64, 180, ELEM_DIRT);   // tube walls, down into the pool
    fill(w, 61, 11, 63, 180, ELEM_VOID);   // tube interior: vacuum (water at the bottom below)
    fill(w, 61, 162, 63, 180, ELEM_WATER);
    fill_air(w, 1, 80, 59, 161, 0.5f);
    fill_air(w, 65, 80, 126, 161, 0.5f);
    sim_run_ticks(w, 1000);
    int pool = water_top(w, 20, 80, 190), tube = water_top(w, 62, 11, 190);
    double head = water_head_cells(mean_pressure(w, 1, 80, 59, 161));
    EXPECT(pool > 0 && tube > 0);
    EXPECT(head > 95.0 && head < 105.0); // 0.5 atm, a little less as the air expands
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
    double air0 = test_mass(w, ELEM_GAS);
    double p0 = mean_pressure(w, 20, 140, 80, 167);
    sim_run_ticks(w, 3000);
    int room = water_top(w, 50, 140, 180), column = water_top(w, 91, 20, 180);
    double p1 = mean_pressure(w, 20, 140, 80, 180);
    double air1 = test_mass(w, ELEM_GAS);
    EXPECT(room > 0 && room < 168); // water came in
    EXPECT(p1 > 1.1 * p0);          // the air was compressed
    EXPECT(fabs(air1 - air0) < 1e-4 * air0);
    EXPECT(fabs((room - column) - water_head_cells(p1)) < 3.0);
    world_destroy(w);
}
