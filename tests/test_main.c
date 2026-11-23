#include "test.h"

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
void test_edit_single_pixel(void);
void test_edit_circle_area(void);
void test_edit_stroke_has_no_gaps(void);
void test_edit_only_void(void);
void test_edit_erase(void);

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        int before = g_test_failures;                                                              \
        g_test_current = #fn;                                                                      \
        fn();                                                                                      \
        printf("%s %s\n", g_test_failures == before ? "ok  " : "FAIL", #fn);                       \
    } while (0)

int main(void) {
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
    RUN(test_edit_single_pixel);
    RUN(test_edit_circle_area);
    RUN(test_edit_stroke_has_no_gaps);
    RUN(test_edit_only_void);
    RUN(test_edit_erase);
    if (g_test_failures) {
        printf("\n%d check(s) failed\n", g_test_failures);
        return 1;
    }
    printf("\nAll tests passed\n");
    return 0;
}
