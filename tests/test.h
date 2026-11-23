#pragma once

// Tiny test framework: each test is `void test_x(void)`, registered in test_main.c.

#include "sim/world.h"

#include <stdio.h>

extern int g_test_failures;
extern const char *g_test_current;

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            g_test_failures++;                                                                     \
            printf("  FAIL %s (%s:%d): %s\n", g_test_current, __FILE__, __LINE__, #cond);          \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(a, b)                                                                            \
    do {                                                                                           \
        long long _a = (long long)(a), _b = (long long)(b);                                        \
        if (_a != _b) {                                                                            \
            g_test_failures++;                                                                     \
            printf("  FAIL %s (%s:%d): %s == %s (%lld vs %lld)\n", g_test_current, __FILE__,       \
                   __LINE__, #a, #b, _a, _b);                                                      \
        }                                                                                          \
    } while (0)

// Builds a world from an ASCII sketch placed at the top-left. Everything outside the sketch is
// dirt, so sketches are enclosed. Legend:
//   ' ' or '.' void   '#' dirt   'R' rock   'I' iron   'i' ice
//   '~' water         'o' oxygen 'c' CO2    'h' hydrogen
World *test_world_from_ascii(const char *ascii, uint64_t seed);

// Builds a world of the given size: dirt everywhere, then an open void interior inside a wall of
// thickness 1.
World *test_world_box(int w, int h, uint64_t seed);

bool test_cell_is(const World *w, int x, int y, ElementId e);
int64_t test_count(const World *w, ElementId e);
// Mean y of all cells of element e (+y is down), weighted by mass for gases. -1 if none.
double test_mean_y(const World *w, ElementId e);
// Total mass of gas e, kg.
double test_mass(const World *w, ElementId e);
