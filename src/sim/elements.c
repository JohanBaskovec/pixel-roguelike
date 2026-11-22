#include "sim/elements.h"

// Reference values: docs/ELEMENTS.md. Change both together.
// clang-format off
const ElementDef g_elem_defs[ELEM_COUNT] = {
    //                 name              behavior         density   c         k        spawn T  colour    alpha var spread
    [ELEM_VOID]     = {"Void",           BEHAVIOR_NONE,   0.0f,     0.0f,     0.0f,    0.0f,    0x0B0B10, 255,  3,  0},
    [ELEM_DIRT]     = {"Dirt",           BEHAVIOR_STATIC, 1500.0f,  800.0f,   0.5f,    293.15f, 0x6B4A2E, 255, 18,  0},
    [ELEM_ROCK]     = {"Rock",           BEHAVIOR_STATIC, 2700.0f,  790.0f,   2.8f,    293.15f, 0x6E6E73, 255, 14,  0},
    [ELEM_IRON]     = {"Iron",           BEHAVIOR_STATIC, 7874.0f,  449.0f,   80.0f,   293.15f, 0x8A8F96, 255,  8,  0},
    [ELEM_ICE]      = {"Ice",            BEHAVIOR_STATIC, 917.0f,   2100.0f,  2.2f,    263.15f, 0xBFE6F5, 255, 10,  0},
    [ELEM_WATER]    = {"Water",          BEHAVIOR_LIQUID, 1000.0f,  4186.0f,  0.6f,    293.15f, 0x2A5DB0, 217, 10,  5},
    [ELEM_OXYGEN]   = {"Oxygen",         BEHAVIOR_GAS,    1.429f,   918.0f,   0.026f,  293.15f, 0x9CC8F0, 110, 30,  3},
    [ELEM_CO2]      = {"Carbon dioxide", BEHAVIOR_GAS,    1.977f,   844.0f,   0.0166f, 293.15f, 0x7A7A7A, 140, 30,  2},
    [ELEM_HYDROGEN] = {"Hydrogen",       BEHAVIOR_GAS,    0.0899f,  14300.0f, 0.18f,   293.15f, 0xE9A6E0, 110, 30,  6},
};
// clang-format on

uint8_t g_elem_behavior[256];
uint32_t g_elem_density_q[256];
uint8_t g_elem_spread[256];
uint16_t g_buoy_p[ELEM_COUNT][ELEM_COUNT];

void elements_init(void) {
    static bool initialised = false;
    if (initialised) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        g_elem_behavior[i] = BEHAVIOR_STATIC; // unknown IDs and ELEM_OUT behave as walls
        g_elem_density_q[i] = UINT32_MAX;
        g_elem_spread[i] = 0;
    }
    for (int i = 0; i < ELEM_COUNT; i++) {
        const ElementDef *d = &g_elem_defs[i];
        PR_ASSERT_ALWAYS(d->spread <= MAX_MOVE_CELLS);
        g_elem_behavior[i] = (uint8_t)d->behavior;
        g_elem_density_q[i] = (uint32_t)(d->density * 1000.0f + 0.5f);
        g_elem_spread[i] = d->spread;
    }
    for (int above = 0; above < ELEM_COUNT; above++) {
        for (int self = 0; self < ELEM_COUNT; self++) {
            double ra = (double)g_elem_defs[above].density;
            double rs = (double)g_elem_defs[self].density;
            double p = 0.0;
            if (ra > rs && ra + rs > 0.0) {
                p = (double)K_BUOY_16 * (ra - rs) / (ra + rs);
            }
            g_buoy_p[above][self] = (uint16_t)PR_MIN(p, 65535.0);
        }
    }
    initialised = true;
}
