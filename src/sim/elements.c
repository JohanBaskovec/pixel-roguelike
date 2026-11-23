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
    [ELEM_STEAM]    = {"Steam",          BEHAVIOR_GAS,    0.59f,    2080.0f,  0.025f,  383.15f, 0xE6ECF2, 64,  30,  4},
    [ELEM_NITROGEN] = {"Nitrogen",       BEHAVIOR_GAS,    1.2506f,  1040.0f,  0.026f,  293.15f, 0x7C86A8, 40,  30,  3},
};
// clang-format on

// clang-format off
const PhaseChange g_elem_below[ELEM_COUNT] = {
    [ELEM_WATER] = {273.15f, ELEM_ICE,   334e3f},  // freezing
    [ELEM_STEAM] = {373.15f, ELEM_WATER, 2257e3f}, // condensing
};
const PhaseChange g_elem_above[ELEM_COUNT] = {
    [ELEM_ICE]   = {273.15f, ELEM_WATER, 334e3f},  // melting
    [ELEM_WATER] = {373.15f, ELEM_STEAM, 2257e3f}, // boiling
};
// clang-format on

// Molar masses of gases, kg/mol.
static const float k_molar_mass[ELEM_COUNT] = {
    [ELEM_OXYGEN] = 0.031999f, [ELEM_CO2] = 0.04401f,       [ELEM_HYDROGEN] = 0.002016f,
    [ELEM_STEAM] = 0.018015f,  [ELEM_NITROGEN] = 0.028014f,
};

// Reference temperature of a gas's tabulated density (STP is 273.15 K; steam is given at 100 °C).
static float gas_ref_temp(ElementId e) {
    return e == ELEM_STEAM ? 373.15f : 273.15f;
}

// Density a gas is given for buoyancy, at gas_ref_temp (see AIR_DENSITY_0C).
static float gas_buoy_density(ElementId e) {
    return (e == ELEM_OXYGEN || e == ELEM_NITROGEN) ? AIR_DENSITY_0C : g_elem_defs[e].density;
}

// Buoyancy density of any element; gases at 0 degC, so that two gases at one temperature compare
// exactly (liquids and solids: their table density).
static double buoy_ref_density(int e) {
    if (g_elem_defs[e].behavior != BEHAVIOR_GAS) {
        return (double)g_elem_defs[e].density;
    }
    return (double)gas_buoy_density((ElementId)e) * (double)gas_ref_temp((ElementId)e) / 273.15;
}

uint8_t g_elem_behavior[256];
uint32_t g_elem_density_q[256];
uint8_t g_elem_spread[256];
uint16_t g_buoy_p[ELEM_COUNT][ELEM_COUNT];
float g_elem_cap[256];
float g_elem_inv_cap[256];
float g_elem_mass[256];
float g_elem_low_t[256];
float g_elem_high_t[256];
float g_heat_coef[ELEM_COUNT][ELEM_COUNT];
float g_heat_coef_raw[ELEM_COUNT][ELEM_COUNT];
float g_elem_molar_mass[256];
float g_gas_inv_molar[256];
float g_gas_mass_atm[256];
float g_gas_split_mass[256];
float g_elem_cp[256];

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
            // For two cells at the same temperature (buoyancy_p computes the general case).
            double ra = buoy_ref_density(above);
            double rs = buoy_ref_density(self);
            double p = 0.0;
            if (ra > rs && ra + rs > 0.0) {
                p = (double)K_BUOY_16 * (ra - rs) / (ra + rs);
            }
            g_buoy_p[above][self] = (uint16_t)PR_MIN(p, 65535.0);
        }
    }
    for (int i = 0; i < ELEM_COUNT; i++) {
        const ElementDef *d = &g_elem_defs[i];
        g_elem_mass[i] = d->density * CELL_VOLUME_M3;
        g_elem_cap[i] = g_elem_mass[i] * d->heat_cap;
        g_elem_inv_cap[i] = g_elem_cap[i] > 0.0f ? 1.0f / g_elem_cap[i] : 0.0f;
        g_elem_low_t[i] = g_elem_below[i].temp;
        g_elem_high_t[i] = g_elem_above[i].temp;
        g_elem_cp[i] = d->heat_cap;
        if (d->behavior == BEHAVIOR_GAS) {
            g_elem_molar_mass[i] = k_molar_mass[i];
            g_gas_inv_molar[i] = 1.0f / k_molar_mass[i];
            g_gas_mass_atm[i] = gas_mass_at((ElementId)i, P_ATM, T_ROOM);
            g_gas_split_mass[i] = 2.0f * GAS_MIN_FRAC * g_gas_mass_atm[i];
            // A gas cell's thermal mass depends on its actual mass (the heat pass uses that);
            // the table value is the nominal one at 1 atm and 20 degC.
            g_elem_mass[i] = g_gas_mass_atm[i];
            g_elem_cap[i] = g_elem_mass[i] * d->heat_cap;
            g_elem_inv_cap[i] = 1.0f / g_elem_cap[i];
        }
    }
    // Pairwise heat exchange: conductance of two half-cells in series (harmonic mean of k) across
    // a face of CELL_SIZE^2 at distance CELL_SIZE, i.e. k_eff * CELL_SIZE, times the (sped-up)
    // tick. Limited so that one tick never pushes two cells past their common temperature even
    // if all four neighbours push the same way: |Q| <= dT * (Ci * Cj / (Ci + Cj)) / 4.
    for (int a = 0; a < ELEM_COUNT; a++) {
        for (int b = 0; b < ELEM_COUNT; b++) {
            float ka = g_elem_defs[a].conduct, kb = g_elem_defs[b].conduct;
            float ca = g_elem_cap[a], cb = g_elem_cap[b];
            float coef = 0.0f;
            if (ka > 0.0f && kb > 0.0f && ca > 0.0f && cb > 0.0f) {
                float k_eff = 2.0f * ka * kb / (ka + kb);
                bool gas = g_elem_defs[a].behavior == BEHAVIOR_GAS ||
                           g_elem_defs[b].behavior == BEHAVIOR_GAS;
                float speedup = gas ? HEAT_TIME_SCALE_GAS : HEAT_TIME_SCALE;
                coef = k_eff * CELL_SIZE_M * TICK_SECONDS * speedup;
                g_heat_coef_raw[a][b] = coef;
                float limit = (ca * cb / (ca + cb)) * 0.25f;
                coef = coef < limit ? coef : limit;
            }
            g_heat_coef[a][b] = coef;
        }
    }
    initialised = true;
}

float elem_phase_progress(ElementId e, float t, bool *heating) {
    if (e >= ELEM_COUNT) {
        return -1.0f;
    }
    const ElementDef *d = &g_elem_defs[e];
    const PhaseChange *up = &g_elem_above[e], *down = &g_elem_below[e];
    if (up->temp > 0.0f && t > up->temp) {
        *heating = true;
        return PR_MIN(d->heat_cap * (t - up->temp) / up->latent, 0.999f);
    }
    if (down->temp > 0.0f && t < down->temp) {
        *heating = false;
        return PR_MIN(d->heat_cap * (down->temp - t) / down->latent, 0.999f);
    }
    return -1.0f;
}

float elem_fluid_density(ElementId e, float t) {
    const ElementDef *d = &g_elem_defs[e < ELEM_COUNT ? e : 0];
    if (d->behavior == BEHAVIOR_GAS) {
        return gas_buoy_density(e) * gas_ref_temp(e) / PR_MAX(t, 1.0f);
    }
    if (e == ELEM_WATER) {
        // Thiesen's formula for pure water, 0-100 degC.
        float c = PR_CLAMP(t - 273.15f, 0.0f, 100.0f);
        float k = (c + 288.9414f) / (508929.2f * (c + 68.12963f));
        return 1000.0f * (1.0f - k * (c - 3.9863f) * (c - 3.9863f));
    }
    if (d->behavior == BEHAVIOR_LIQUID) {
        return d->density * (1.0f - 2e-4f * (t - 293.15f));
    }
    return d->density;
}
