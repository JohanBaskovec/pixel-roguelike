#include "test.h"

#include "sim/edit.h"
#include "sim/sim.h"

#include <math.h>

void test_wood_burns_in_air(void);
void test_wood_needs_oxygen(void);
void test_wood_burns_only_above_16_percent_oxygen(void);

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT};
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
