#include "test.h"

#include "core/common.h"

#include <string.h>

int g_test_failures = 0;
const char *g_test_current = "";

void test_solids_never_move(void);
void test_water_falls_and_levels(void);
void test_water_sinks_through_gas(void);
void test_gas_expands_into_void(void);
void test_co2_settles_below_oxygen(void);
void test_hydrogen_rises_above_oxygen(void);
void test_counts_conserved(void);
void test_deterministic(void);
void test_settled_world_sleeps(void);
void test_no_diagonal_leak(void);
void test_poured_water_spreads(void);
void test_falling_stream_has_no_chunk_bulges(void);
void test_pour_against_wall_makes_no_tower(void);
void test_same_result_for_any_thread_count(void);
void test_u_tube_balances(void);
void test_water_flows_through_hole_until_level(void);
void test_heat_conserves_energy(void);
void test_heat_never_overshoots(void);
void test_iron_conducts_faster_than_rock(void);
void test_ice_melts_in_warm_water(void);
void test_phase_changes_need_latent_heat(void);
void test_hot_gas_rises(void);
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
void test_particle_flies_ballistic(void);
void test_deep_reservoir_jets_further(void);
void test_fast_landing_splashes(void);
void test_jets_same_for_any_thread_count(void);
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
void test_sand_piles_up(void);
void test_sand_sinks_in_water(void);
void test_digging_makes_rubble(void);
void test_creature_stands_on_sand(void);
void test_rock_and_iron_melt_and_freeze(void);
void test_lava_boils_water(void);
void test_oil_floats_on_water(void);
void test_oil_manometer(void);
void test_hydrogen_burns_in_oxygen(void);
void test_cold_hydrogen_does_not_burn(void);
void test_burning_same_for_any_thread_count(void);
void test_rifle_digs_dirt(void);
void test_bullet_stops_at_iron(void);
void test_bullet_stirs_air(void);
void test_shooting_same_for_any_thread_count(void);
void test_small_clumps_fall(void);
void test_player_runs_and_jumps(void);
void test_air_brush_spreads_oxygen(void);
void test_walk_forward_while_shooting(void);
void test_walking_kicks_small_piles(void);


void test_heater_makes_plume(void);
void test_pool_heated_from_below_convects(void);
void test_moved_water_carries_heat(void);
void test_edit_single_pixel(void);
void test_edit_circle_area(void);
void test_edit_stroke_has_no_gaps(void);
void test_edit_only_void(void);
void test_edit_erase(void);

// Only tests whose name contains this (the first argument), or all; with --exact, only the test
// of exactly that name (how CTest runs each test on its own, in parallel).
static const char *s_filter;
static bool s_exact;
static int s_ran;

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        if (s_filter && (s_exact ? strcmp(#fn, s_filter) != 0 : !strstr(#fn, s_filter))) {         \
            break;                                                                                 \
        }                                                                                          \
        int before = g_test_failures;                                                              \
        g_test_current = #fn;                                                                      \
        double t0 = pr_time_seconds();                                                             \
        fn();                                                                                      \
        printf("%s %-48s %6.2f s\n", g_test_failures == before ? "ok  " : "FAIL", #fn,             \
               pr_time_seconds() - t0);                                                            \
        s_ran++;                                                                                   \
    } while (0)

// Usage: pr-tests [name-filter] | pr-tests --exact name
int main(int argc, char **argv) {
    s_exact = argc > 2 && strcmp(argv[1], "--exact") == 0;
    s_filter = s_exact ? argv[2] : argc > 1 ? argv[1] : NULL;
    RUN(test_solids_never_move);
    RUN(test_water_falls_and_levels);
    RUN(test_water_sinks_through_gas);
    RUN(test_gas_expands_into_void);
    RUN(test_co2_settles_below_oxygen);
    RUN(test_hydrogen_rises_above_oxygen);
    RUN(test_counts_conserved);
    RUN(test_deterministic);
    RUN(test_settled_world_sleeps);
    RUN(test_no_diagonal_leak);
    RUN(test_poured_water_spreads);
    RUN(test_falling_stream_has_no_chunk_bulges);
    RUN(test_pour_against_wall_makes_no_tower);
    RUN(test_same_result_for_any_thread_count);
    RUN(test_u_tube_balances);
    RUN(test_water_flows_through_hole_until_level);
    RUN(test_heat_conserves_energy);
    RUN(test_heat_never_overshoots);
    RUN(test_iron_conducts_faster_than_rock);
    RUN(test_ice_melts_in_warm_water);
    RUN(test_phase_changes_need_latent_heat);
    RUN(test_hot_gas_rises);
    RUN(test_heater_makes_plume);
    RUN(test_gas_fills_vacuum);
    RUN(test_gas_pressure_equalizes);
    RUN(test_sealed_gas_pressure_follows_temperature);
    RUN(test_boiling_keeps_mass);
    RUN(test_condensing_keeps_mass_on_average);
    RUN(test_air_stays_mixed);
    RUN(test_gases_mix_and_settle);
    RUN(test_gas_mixture_stays_mixed);
    RUN(test_co2_blob_sinks_and_pools);
    RUN(test_dilute_co2_stays_in_air);
    RUN(test_bulk_buoyancy_same_for_any_thread_count);
    RUN(test_pressurised_tank_drives_water_up_a_pipe);
    RUN(test_air_holds_water_up_a_vacuum_tube);
    RUN(test_trapped_air_is_compressed);
    RUN(test_particle_flies_ballistic);
    RUN(test_deep_reservoir_jets_further);
    RUN(test_fast_landing_splashes);
    RUN(test_jets_same_for_any_thread_count);
    RUN(test_creature_falls_and_stands);
    RUN(test_creature_walks_and_steps_up);
    RUN(test_creature_stirs_gas_and_conserves_it);
    RUN(test_creature_floats_in_water);
    RUN(test_creature_breathes_at_real_rates);
    RUN(test_creature_suffocates_in_sealed_room);
    RUN(test_creature_body_heat);
    RUN(test_creatures_same_for_any_thread_count);
    RUN(test_walking_stirs_air);
    RUN(test_falling_sand_stirs_air);
    RUN(test_sand_piles_up);
    RUN(test_sand_sinks_in_water);
    RUN(test_digging_makes_rubble);
    RUN(test_creature_stands_on_sand);
    RUN(test_rock_and_iron_melt_and_freeze);
    RUN(test_lava_boils_water);
    RUN(test_oil_floats_on_water);
    RUN(test_oil_manometer);
    RUN(test_hydrogen_burns_in_oxygen);
    RUN(test_cold_hydrogen_does_not_burn);
    RUN(test_burning_same_for_any_thread_count);
    RUN(test_rifle_digs_dirt);
    RUN(test_bullet_stops_at_iron);
    RUN(test_bullet_stirs_air);
    RUN(test_shooting_same_for_any_thread_count);
    RUN(test_small_clumps_fall);
    RUN(test_player_runs_and_jumps);
    RUN(test_air_brush_spreads_oxygen);
    RUN(test_walk_forward_while_shooting);
    RUN(test_walking_kicks_small_piles);


    RUN(test_pool_heated_from_below_convects);
    RUN(test_moved_water_carries_heat);
    RUN(test_edit_single_pixel);
    RUN(test_edit_circle_area);
    RUN(test_edit_stroke_has_no_gaps);
    RUN(test_edit_only_void);
    RUN(test_edit_erase);
    if (s_exact && s_ran == 0) {
        printf("No test named %s\n", s_filter);
        return 1;
    }
    if (g_test_failures) {
        printf("\n%d check(s) failed\n", g_test_failures);
        return 1;
    }
    printf("\nAll tests passed\n");
    return 0;
}
