#include "sim/elements.h"

#include <math.h>

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
    [ELEM_STEAM]    = {"Water vapour",   BEHAVIOR_GAS,    0.59f,    2080.0f,  0.025f,  383.15f, 0xE6ECF2, 64,  30,  4},
    [ELEM_NITROGEN] = {"Nitrogen",       BEHAVIOR_GAS,    1.2506f,  1040.0f,  0.026f,  293.15f, 0x7C86A8, 40,  30,  3},
    [ELEM_BODY]     = {"Body",           BEHAVIOR_STATIC, 985.0f,   3500.0f,  0.5f,    310.15f, 0xD9A066, 255, 12,  0},
    [ELEM_SAND]     = {"Sand",           BEHAVIOR_POWDER, 1600.0f,  830.0f,   0.25f,   293.15f, 0xC2B280, 255, 16,  0},
    [ELEM_SOIL]     = {"Loose soil",     BEHAVIOR_POWDER, 1200.0f,  800.0f,   0.3f,    293.15f, 0x7E5A3A, 255, 20,  0},
    [ELEM_RUBBLE]   = {"Rubble",         BEHAVIOR_POWDER, 1700.0f,  790.0f,   0.5f,    293.15f, 0x85858B, 255, 22,  0},
    [ELEM_LAVA]     = {"Lava",           BEHAVIOR_LIQUID, 2600.0f,  1100.0f,  1.5f,    1600.0f, 0xC8461E, 255, 14,  1},
    [ELEM_MOLTEN_IRON] = {"Molten iron",  BEHAVIOR_LIQUID, 6980.0f,  820.0f,   35.0f,   1900.0f, 0xD07830, 255, 10,  3},
    [ELEM_OIL]      = {"Oil",            BEHAVIOR_LIQUID, 870.0f,   2000.0f,  0.15f,   293.15f, 0x3C2A12, 235, 10,  3},
    [ELEM_BULLET]   = {"Bullet",         BEHAVIOR_POWDER, 11340.0f, 129.0f,   35.0f,   293.15f, 0x50545C, 255, 10,  0},
    [ELEM_CASING]   = {"Casing",         BEHAVIOR_POWDER, 8500.0f,  380.0f,   110.0f,  293.15f, 0xC9A23A, 255, 18,  0},
    [ELEM_LEAD]     = {"Lead",           BEHAVIOR_STATIC, 11340.0f, 129.0f,   35.0f,   293.15f, 0x5A5E66, 255,  6,  0},
    [ELEM_MOLTEN_LEAD] = {"Molten lead", BEHAVIOR_LIQUID, 10660.0f, 142.0f,   16.0f,   700.0f,  0xA8AEB8, 255,  6,  4},
    [ELEM_BRASS]    = {"Brass",          BEHAVIOR_STATIC, 8500.0f,  380.0f,   110.0f,  293.15f, 0xB8923A, 255,  8,  0},
    [ELEM_MOLTEN_BRASS] = {"Molten brass", BEHAVIOR_LIQUID, 7600.0f, 470.0f,  70.0f,   1300.0f, 0xD8A848, 255,  8,  3},
    [ELEM_BLOOD]    = {"Blood",          BEHAVIOR_LIQUID, 1060.0f,  3600.0f,  0.5f,    293.15f, 0x8A0F14, 235, 12,  3},
    // A gas cell: its properties come from its mixture; these (air's) stand for any gas in the
    // tables (buoyancy against liquids, conduction tables).
    [ELEM_GAS]      = {"Gas",            BEHAVIOR_GAS,    1.2922f,  1005.0f,  0.026f,  293.15f, 0x9CC8F0, 110, 30,  3},
};
// clang-format on

// clang-format off
const PhaseChange g_elem_below[ELEM_COUNT] = {
    [ELEM_WATER]       = {273.15f, ELEM_ICE,   334e3f},  // freezing (vapour condenses: sim.c)
    [ELEM_LAVA]        = {1500.0f, ELEM_ROCK,  400e3f},  // solidifying
    [ELEM_MOLTEN_IRON] = {1811.0f, ELEM_IRON,  247e3f},  // solidifying
    [ELEM_MOLTEN_LEAD] = {600.6f,  ELEM_LEAD,  23.0e3f}, // solidifying
    [ELEM_MOLTEN_BRASS] = {1208.0f, ELEM_BRASS, 168e3f}, // solidifying
};
const PhaseChange g_elem_above[ELEM_COUNT] = {
    [ELEM_ICE]   = {273.15f, ELEM_WATER,       334e3f},  // melting
    [ELEM_WATER] = {373.15f, ELEM_GAS,         2257e3f}, // boiling: into water vapour
    [ELEM_ROCK]  = {1500.0f, ELEM_LAVA,        400e3f},  // melting
    [ELEM_IRON]  = {1811.0f, ELEM_MOLTEN_IRON, 247e3f},  // melting
    [ELEM_BULLET] = {600.6f, ELEM_MOLTEN_LEAD, 23.0e3f}, // melting
    [ELEM_LEAD]  = {600.6f,  ELEM_MOLTEN_LEAD, 23.0e3f}, // melting
    [ELEM_CASING] = {1208.0f, ELEM_MOLTEN_BRASS, 168e3f}, // melting
    [ELEM_BRASS] = {1208.0f, ELEM_MOLTEN_BRASS, 168e3f}, // melting
};
// clang-format on

// 2 H2 + O2 -> 2 H2O: 8 kg of O2 per kg of H2, 120 MJ per kg of H2 (lower heating value: the
// water leaves as steam). Hydrogen autoignites in air at about 773 K.
const Reaction g_reactions[REACTION_COUNT] = {
    {ELEM_HYDROGEN, ELEM_OXYGEN, ELEM_STEAM, 773.0f, 7.937f, 120e6f},
};
uint8_t g_reaction_of[256];
uint8_t g_gas_slot[256];
float g_min_ignite_k = 1e30f;
const ElementId g_species_elem[GAS_SPECIES] = {ELEM_OXYGEN, ELEM_CO2, ELEM_HYDROGEN, ELEM_STEAM,
                                               ELEM_NITROGEN};
float g_species_molar[GAS_SPECIES];
float g_species_inv_molar[GAS_SPECIES];
float g_species_cp[GAS_SPECIES];
float g_species_k[GAS_SPECIES];

// Molar masses of gases, kg/mol.
static const float k_molar_mass[ELEM_COUNT] = {
    [ELEM_OXYGEN] = 0.031999f, [ELEM_CO2] = 0.04401f,       [ELEM_HYDROGEN] = 0.002016f,
    [ELEM_STEAM] = 0.018015f,  [ELEM_NITROGEN] = 0.028014f,
};

// Buoyancy density of any element (gases: ELEM_GAS as air at 0 degC; species: at 0 degC, steam
// given at 100 degC), for the table of fluid pairs (liquids against gas).
static double buoy_ref_density(int e) {
    if (e == ELEM_STEAM) {
        return (double)g_elem_defs[e].density * 373.15 / 273.15;
    }
    return (double)g_elem_defs[e].density;
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
float g_gas_mass_atm[256];
float g_elem_cp[256];

// Antoine's equation: log10(p / mmHg) = A - B / (C + T / degC).
static float antoine(float t) {
    float c = t - 273.15f;
    float lg =
        c < 99.0f ? 8.07131f - 1730.63f / (233.426f + c) : 8.14019f - 1810.94f / (244.485f + c);
    return powf(10.0f, lg) * 133.322f;
}

// Tabulated at 1 K steps from SAT_T0 (it is read for every gas cell holding vapour).
#define SAT_T0 150
#define SAT_N  500
static float g_sat[SAT_N + 1];

float water_saturation_pressure(float t) {
    float u = PR_CLAMP(t - (float)SAT_T0, 0.0f, (float)SAT_N - 0.001f);
    int k = (int)u;
    float f = u - (float)k;
    return g_sat[k] + (g_sat[k + 1] - g_sat[k]) * f;
}

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
    for (int s = 0; s < GAS_SPECIES; s++) {
        ElementId e = g_species_elem[s];
        g_gas_slot[e] = (uint8_t)(s + 1);
        g_species_molar[s] = k_molar_mass[e];
        g_species_inv_molar[s] = 1.0f / k_molar_mass[e];
        g_species_cp[s] = g_elem_defs[e].heat_cap;
        g_species_k[s] = g_elem_defs[e].conduct;
    }
    for (int r = 0; r < REACTION_COUNT; r++) {
        g_reaction_of[g_reactions[r].a] = (uint8_t)(r + 1);
        g_min_ignite_k = PR_MIN(g_min_ignite_k, g_reactions[r].ignite_k);
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
            // A gas cell's thermal mass depends on its mixture (the heat pass uses that); the
            // table value is the nominal one at 1 atm and 20 degC (air for ELEM_GAS).
            g_elem_molar_mass[i] = i == ELEM_GAS ? 0.02897f : k_molar_mass[i];
            g_gas_mass_atm[i] = gas_mass_at((ElementId)i, P_ATM, T_ROOM);
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
    for (int k = 0; k <= SAT_N; k++) {
        g_sat[k] = antoine((float)(SAT_T0 + k));
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
        return (float)buoy_ref_density(e) * 273.15f / PR_MAX(t, 1.0f);
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
