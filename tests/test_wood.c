#include "test.h"

#include "sim/edit.h"
#include "sim/sim.h"

#include <math.h>

void test_wood_burns_in_air(void);
void test_hot_gas_lights_wood(void);
void test_embers_heat_the_wood_beside_them(void);
void test_wood_needs_oxygen(void);
void test_wood_burns_only_above_16_percent_oxygen(void);

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT, 0.0f, 0.0f};
        sim_apply_edit(w, &ed);
    }
}

// A log (11 x 17 cells) standing on the floor of a room, lit at one cell of its top.
static void light(World *w) {
    w->temp[world_index(w, 45, 110)] = 700.0f;
    world_wake_heat(w, 30, 100, 60, 126);
    world_wake_rect(w, 30, 100, 60, 126);
}

void test_wood_burns_in_air(void) {
    // Lit at one cell, a log burns on in air: the fire spreads through the wood, takes oxygen out
    // of the air, and puts carbon dioxide and water vapour in: 1.185 kg of O2 per kg of wood
    // gives 1.629 kg of CO2 and 0.556 kg of H2O (cellulose), so the three change by those ratios.
    World *w = test_world_box(128, 128, 140);
    fill(w, 1, 1, 126, 126, EDIT_AIR);
    fill(w, 40, 110, 50, 126, ELEM_WOOD);
    int64_t wood0 = test_count(w, ELEM_WOOD);
    double o2 = test_mass(w, ELEM_OXYGEN), co2 = test_mass(w, ELEM_CO2),
           h2o = test_mass(w, ELEM_STEAM);
    light(w);
    sim_run_ticks(w, 900);
    int64_t gone = wood0 - test_count(w, ELEM_WOOD); // burning or burnt
    EXPECT(gone >= 6);                                // it spread beyond the cell lit
    EXPECT(test_count(w, ELEM_BURNING_WOOD) >= 3);
    double d_o2 = o2 - test_mass(w, ELEM_OXYGEN), d_co2 = test_mass(w, ELEM_CO2) - co2,
           d_h2o = test_mass(w, ELEM_STEAM) - h2o;
    EXPECT(d_o2 > 0.01);
    EXPECT(fabs(d_co2 / d_o2 - 1.6293 / 1.18519) < 0.03);
    EXPECT(fabs(d_h2o / d_o2 - 0.55602 / 1.18519) < 0.03);
    world_destroy(w);
}

void test_wood_needs_oxygen(void) {
    // In nitrogen the wood lit at one cell does not burn: it makes no carbon dioxide, and the
    // cell goes out as it cools.
    World *w = test_world_box(128, 128, 141);
    fill(w, 1, 1, 126, 126, ELEM_NITROGEN);
    fill(w, 40, 110, 50, 126, ELEM_WOOD);
    int64_t wood0 = test_count(w, ELEM_WOOD);
    light(w);
    sim_run_ticks(w, 900);
    EXPECT_EQ(test_count(w, ELEM_WOOD), wood0); // none burnt away, none burning
    EXPECT_EQ(test_count(w, ELEM_BURNING_WOOD), 0);
    EXPECT(test_mass(w, ELEM_CO2) < 1e-9);
    world_destroy(w);
}

// A room filled with `o2` (mole fraction) oxygen and the rest nitrogen, at 1 atm and 20 degC, with a
// log lit at one cell; returns the wood cells that burnt or are burning after `ticks`.
static int64_t burnt_at(double o2, int ticks, uint64_t seed) {
    World *w = test_world_box(128, 128, seed);
    GasMix mix;
    gas_mix_clear(&mix);
    float cell_mol = P_ATM * CELL_VOLUME_M3 / (GAS_R * T_ROOM);
    mix.m[GAS_O2] = (float)o2 * cell_mol * g_species_molar[GAS_O2];
    mix.m[GAS_N2] = (float)(1.0 - o2) * cell_mol * g_species_molar[GAS_N2];
    for (int y = 1; y <= 126; y++) {
        for (int x = 1; x <= 126; x++) {
            world_set_mix(w, x, y, &mix, T_ROOM, (uint32_t)(x * 31 + y));
        }
    }
    fill(w, 40, 110, 50, 126, ELEM_WOOD);
    int64_t wood0 = test_count(w, ELEM_WOOD);
    light(w);
    sim_run_ticks(w, ticks);
    int64_t gone = wood0 - test_count(w, ELEM_WOOD);
    world_destroy(w);
    return gone;
}

void test_wood_burns_only_above_16_percent_oxygen(void) {
    // Air with 21% oxygen lets it burn; with 14% it does not (wood stops burning below about 16%).
    int64_t at21 = burnt_at(0.21, 700, 142);
    int64_t at14 = burnt_at(0.14, 700, 143);
    EXPECT(at21 >= 3);
    EXPECT_EQ(at14, 0);
}

void test_hot_gas_lights_wood(void) {
    // A wood cell in gas that is held at 1300 K (a steady flame; a puff of hot air alone holds too
    // little heat to matter) is heated by it WOOD_GAS_COUPLING times faster than still air
    // conducts, and catches fire within seconds.
    World *w = test_world_box(64, 64, 144);
    fill(w, 1, 1, 62, 62, EDIT_AIR);
    fill(w, 25, 43, 25, 43, ELEM_WOOD);
    float peak = 0.0f;
    int t_lit = -1;
    for (int t = 0; t < 600 && t_lit < 0; t++) {
        for (int y = 38; y <= 48; y++) {
            for (int x = 20; x <= 30; x++) {
                size_t i = world_index(w, x, y);
                if (w->cells[i].element == ELEM_GAS) {
                    w->temp[i] = 1300.0f;
                }
            }
        }
        world_wake_heat(w, 18, 36, 32, 50);
        sim_tick(w, NULL);
        peak = fmaxf(peak, w->temp[world_index(w, 25, 43)]);
        if (w->cells[world_index(w, 25, 43)].element == ELEM_BURNING_WOOD) {
            t_lit = t;
        }
    }
    EXPECT(t_lit >= 0 && t_lit < 600);
    world_destroy(w);
}

void test_embers_heat_the_wood_beside_them(void) {
    // A burning cell with no oxygen around it (a smouldering ember inside a log) still passes
    // heat to the wood beside it, faster than plain conduction, and lights it.
    World *w = test_world_box(64, 64, 145);
    fill(w, 1, 1, 62, 62, ELEM_NITROGEN); // no oxygen at all
    fill(w, 20, 50, 30, 62, ELEM_WOOD);
    size_t ember = world_index(w, 25, 55);
    w->cells[ember].element = ELEM_BURNING_WOOD;
    w->temp[ember] = 1400.0f;
    world_wake_heat(w, 15, 45, 35, 62);
    world_wake_rect(w, 15, 45, 35, 62);
    sim_run_ticks(w, 120);
    EXPECT(w->temp[world_index(w, 24, 55)] > 400.0f || w->cells[world_index(w, 24, 55)].element == ELEM_BURNING_WOOD);
    world_destroy(w);
}
