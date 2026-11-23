#include "sim/creature.h"

#include "core/rng.h"
#include "sim/particle.h"

#include <math.h>

// Walking at 1.4 m/s; steps up ledges up to STEP_UP cells (20 cm) without stopping.
#define WALK_SPEED (1.4f / CELL_SIZE_M * TICK_SECONDS)
#define STEP_UP    4
#define FALL_MAX   24.0f // cells per tick
// In liquid: drag per tick on the velocity, times the submerged fraction; swimming upward when
// the head is under (acceleration, cells per tick^2).
#define WATER_DRAG   0.15f
#define SWIM_ACCEL   0.03f
#define BODY_DENSITY 985.0f // kg/m^3 (floats, barely, in fresh water)

// Breathing at rest (docs/SIMULATION.md -> Creatures): 14 breaths a minute; 250 mL/min of O2
// consumed and 200 mL/min of CO2 produced (at 20 degC, 1 atm: 0.333 g and 0.368 g per minute).
// Walking multiplies both (and body heat) by WORK_WALKING.
#define BREATH_TICKS  257 // 60 s / 14 breaths, in ticks
#define O2_KG_PER_S   5.55e-6f
#define CO2_KG_PER_S  6.14e-6f
#define WORK_WALKING  3.0f
#define BREATH_RADIUS 16 // the gas a breath draws from, around the mouth
#define BREATH_CELLS  256
// Partial pressures are measured in this window around the mouth, and smoothed over breaths
// (exponential average, weight AIR_SMOOTHING for the newest): air is one gas per cell, so a small
// window holds few oxygen cells and its reading jumps.
#define HEAD_RADIUS   16
#define AIR_SMOOTHING 0.3f
// Breathing (0.5 L a breath) and the warm air rising from a body move air around a person: each
// breath swaps STIR_CELLS cells (2 L) times the speed-up and work, between gas near the mouth
// (within STIR_NEAR, where breaths draw from) and gas further off (within STIR_FAR, 3.2 m). With
// one gas per cell, mixing at interfaces is all the gases would do otherwise, and the air next to
// the face would go stale while the room stays fresh. The air stirred per gram of oxygen used does
// not depend on the speed-up.
#define STIR_CELLS 16
#define STIR_NEAR  BREATH_RADIUS
#define STIR_FAR   64
// Partial pressures (Pa): below PO2_IMPAIRED (16% of an atmosphere) it is impaired, below
// PO2_DANGER (10%), or above PCO2_DANGER (5%), in danger.
#define PO2_IMPAIRED 16200.0f
#define PO2_DANGER   10100.0f
#define PCO2_DANGER  5070.0f
// In danger it dies after DANGER_SECONDS; it recovers fully in RECOVER_SECONDS.
#define DANGER_SECONDS  120.0f
#define RECOVER_SECONDS 600.0f
// Body heat: held at 37 degC with up to BODY_POWER_W (at rest; times the work factor). Heat
// reaches the air HEAT_TIME_SCALE_GAS times faster than in reality (D-022), so the power is
// scaled the same way.
#define BODY_POWER_W 100.0f

static inline bool body_can_enter(const World *w, int x, int y) {
    ElementId e = world_elem(w, x, y);
    return e != ELEM_OUT && g_elem_behavior[e] != BEHAVIOR_STATIC;
}

// Swaps two cells (matter and the body cell it trades places with), with temperatures and masses.
static void swap_world_cells(World *w, int x0, int y0, int x1, int y1) {
    size_t a = world_index(w, x0, y0), b = world_index(w, x1, y1);
    Cell ca = w->cells[a], cb = w->cells[b];
    ca.stamp = cb.stamp = (uint8_t)(w->tick - 1); // simulated this tick
    w->cells[a] = cb;
    w->cells[b] = ca;
    float t = w->temp[a];
    w->temp[a] = w->temp[b];
    w->temp[b] = t;
    float m = w->mass[a];
    w->mass[a] = w->mass[b];
    w->mass[b] = m;
    world_note_moved(w, x0, y0);
    world_note_moved(w, x1, y1);
}

// Can the body move one cell by (dx, dy)? Every cell its leading edge moves into must be free of
// solids (void, gas or liquid, which it pushes).
static bool can_move(const World *w, const Creature *c, int dx, int dy) {
    if (dx != 0) {
        int lx = dx > 0 ? c->x + CREATURE_W : c->x - 1;
        for (int r = 0; r < CREATURE_H; r++) {
            if (!body_can_enter(w, lx, c->y + r)) {
                return false;
            }
        }
        return true;
    }
    int ly = dy > 0 ? c->y + CREATURE_H : c->y - 1;
    for (int k = 0; k < CREATURE_W; k++) {
        if (!body_can_enter(w, c->x + k, ly)) {
            return false;
        }
    }
    return true;
}

// Moves the body one cell. The matter in each cell it moves into trades places with the body cell
// it leaves at the other end: gas and liquid flow around the body, and nothing is lost.
static bool move(World *w, Creature *c, int dx, int dy) {
    if (!can_move(w, c, dx, dy)) {
        return false;
    }
    if (dx != 0) {
        int lx = dx > 0 ? c->x + CREATURE_W : c->x - 1;
        int tx = dx > 0 ? c->x : c->x + CREATURE_W - 1;
        for (int r = 0; r < CREATURE_H; r++) {
            swap_world_cells(w, lx, c->y + r, tx, c->y + r);
        }
        c->x += dx;
    } else {
        int ly = dy > 0 ? c->y + CREATURE_H : c->y - 1;
        int ty = dy > 0 ? c->y : c->y + CREATURE_H - 1;
        for (int k = 0; k < CREATURE_W; k++) {
            swap_world_cells(w, c->x + k, ly, c->x + k, ty);
        }
        c->y += dy;
    }
    world_wake_rect(w, c->x - 2, c->y - 2, c->x + CREATURE_W + 1, c->y + CREATURE_H + 1);
    return true;
}

// One cell sideways, stepping up a ledge of up to STEP_UP cells if it stands on the ground.
static bool step(World *w, Creature *c, int dx) {
    if (move(w, c, dx, 0)) {
        return true;
    }
    if (!c->grounded) {
        return false;
    }
    int up = 0;
    while (up < STEP_UP && move(w, c, 0, -1)) {
        up++;
        if (move(w, c, dx, 0)) {
            return true;
        }
    }
    while (up-- > 0) { // could not get over it: back down
        move(w, c, 0, 1);
    }
    return false;
}

// Fraction of the cells along both sides of the body that hold liquid.
static float submerged_fraction(const World *w, const Creature *c) {
    int n = 0;
    for (int r = 0; r < CREATURE_H; r++) {
        n += g_elem_behavior[world_elem(w, c->x - 1, c->y + r)] == BEHAVIOR_LIQUID;
        n += g_elem_behavior[world_elem(w, c->x + CREATURE_W, c->y + r)] == BEHAVIOR_LIQUID;
    }
    return (float)n / (float)(2 * CREATURE_H);
}

static float rand01(uint64_t h) {
    return (float)(h >> 40) * (1.0f / 16777216.0f);
}

static void physics(World *w, Creature *c) {
    bool alive = c->state != CREATURE_DEAD;
    c->submerged = submerged_fraction(w, c);
    float f = c->submerged;
    // Gravity, and buoyancy for the submerged part (water: 1000 kg/m^3).
    c->vy += PARTICLE_GRAVITY * (1.0f - f * 1000.0f / BODY_DENSITY);
    if (f > 0.0f) {
        c->vx *= 1.0f - WATER_DRAG * f;
        c->vy *= 1.0f - WATER_DRAG * f;
        int my = c->y + CREATURE_MOUTH;
        bool head_under = g_elem_behavior[world_elem(w, c->x - 1, my)] == BEHAVIOR_LIQUID &&
                          g_elem_behavior[world_elem(w, c->x + CREATURE_W, my)] == BEHAVIOR_LIQUID;
        if (alive && f > 0.5f && head_under) {
            c->vy -= SWIM_ACCEL; // head under: swim up
        }
    }
    c->vy = PR_CLAMP(c->vy, -FALL_MAX, FALL_MAX);
    if (alive && (c->grounded || f > 0.0f)) {
        float speed = WALK_SPEED * (c->state == CREATURE_IMPAIRED ? 0.5f : 1.0f);
        c->vx = (float)c->walk * speed * (f > 0.0f ? 0.5f : 1.0f);
    } else if (!alive && c->grounded) {
        c->vx = 0.0f;
    }

    c->fx += c->vx;
    c->fy += c->vy;
    while (c->fx >= 1.0f || c->fx <= -1.0f) {
        int dx = c->fx > 0.0f ? 1 : -1;
        if (step(w, c, dx)) {
            c->fx -= (float)dx;
        } else {
            c->fx = 0.0f;
            c->vx = 0.0f;
            if (c->wander) {
                c->walk = (int8_t)-c->walk; // blocked: turn around
            }
        }
    }
    while (c->fy >= 1.0f || c->fy <= -1.0f) {
        int dy = c->fy > 0.0f ? 1 : -1;
        if (move(w, c, 0, dy)) {
            c->fy -= (float)dy;
        } else {
            c->fy = 0.0f;
            c->vy = 0.0f;
        }
    }
    c->grounded = !can_move(w, c, 0, 1);
    if (c->grounded && c->vy > 0.0f) {
        c->vy = 0.0f;
    }
}

// The cell the creature breathes from: in front of its face, else behind.
static void mouth_cell(const World *w, const Creature *c, int *mx, int *my) {
    *my = c->y + CREATURE_MOUTH;
    *mx = c->facing > 0 ? c->x + CREATURE_W : c->x - 1;
    if (!body_can_enter(w, *mx, *my)) {
        *mx = c->facing > 0 ? c->x - 1 : c->x + CREATURE_W;
    }
}

// Partial pressures of O2 and CO2 around the mouth: each gas's pressure summed over its cells,
// divided by the cells gas could fill (void and gas; Dalton).
static void measure_air(const World *w, Creature *c, int mx, int my) {
    double o2 = 0.0, co2 = 0.0;
    int n = 0;
    for (int y = my - HEAD_RADIUS; y <= my + HEAD_RADIUS; y++) {
        for (int x = mx - HEAD_RADIUS; x <= mx + HEAD_RADIUS; x++) {
            ElementId e = world_elem(w, x, y);
            uint8_t b = g_elem_behavior[e];
            if (b != BEHAVIOR_NONE && b != BEHAVIOR_GAS) {
                continue;
            }
            n++;
            if (e == ELEM_OXYGEN) {
                o2 += (double)world_gas_pressure(w, x, y);
            } else if (e == ELEM_CO2) {
                co2 += (double)world_gas_pressure(w, x, y);
            }
        }
    }
    float po2 = n ? (float)(o2 / n) : 0.0f, pco2 = n ? (float)(co2 / n) : 0.0f;
    if (c->po2 < 0.0f) { // first measurement
        c->po2 = po2;
        c->pco2 = pco2;
    }
    c->po2 += (po2 - c->po2) * AIR_SMOOTHING;
    c->pco2 += (pco2 - c->pco2) * AIR_SMOOTHING;
}

// Swaps gas cells near the mouth with gas cells further off (see STIR_CELLS).
static void stir(World *w, const Creature *c, int mx, int my, float work) {
    int swaps = (int)((float)STIR_CELLS * c->metabolism_scale * work + 0.5f);
    uint64_t h = pr_splitmix64(w->seed ^ (w->tick * 0xD1B54A32D192ED03ull) ^ c->id);
    for (int k = 0; k < swaps; k++) {
        h = pr_splitmix64(h);
        int ax = mx + (int)(h % (2 * STIR_NEAR + 1)) - STIR_NEAR;
        int ay = my + (int)((h >> 16) % (2 * STIR_NEAR + 1)) - STIR_NEAR;
        int bx = mx + (int)((h >> 32) % (2 * STIR_FAR + 1)) - STIR_FAR;
        int by = my + (int)((h >> 48) % (2 * STIR_FAR + 1)) - STIR_FAR;
        if (g_elem_behavior[world_elem(w, ax, ay)] != BEHAVIOR_GAS ||
            g_elem_behavior[world_elem(w, bx, by)] != BEHAVIOR_GAS) {
            continue;
        }
        swap_world_cells(w, ax, ay, bx, by);
    }
    world_wake_rect(w, mx - STIR_FAR - 1, my - STIR_FAR - 1, mx + STIR_FAR + 1, my + STIR_FAR + 1);
}

// One breath: take the oxygen it uses from the oxygen around the mouth (at most half of each
// cell), breathe out CO2 for what it used, and stir the air.
static void breathe(World *w, Creature *c, float work) {
    int mx, my;
    mouth_cell(w, c, &mx, &my);
    stir(w, c, mx, my, work);
    measure_air(w, c, mx, my);
    float seconds = (float)BREATH_TICKS * TICK_SECONDS * c->metabolism_scale * work;
    float need = O2_KG_PER_S * seconds;

    int32_t found[BREATH_CELLS + 1];
    int n = world_gas_region(w, mx, my, ELEM_OXYGEN, mx, my, BREATH_RADIUS, BREATH_CELLS, found);
    if (world_elem(w, mx, my) == ELEM_OXYGEN) {
        found[n++] = (int32_t)world_index(w, mx, my);
    }
    double total = 0.0;
    for (int k = 0; k < n; k++) {
        total += (double)w->mass[found[k]];
    }
    float take = (float)PR_MIN((double)need, 0.5 * total);
    if (take > 0.0f) {
        float fraction = (float)((double)take / total);
        for (int k = 0; k < n; k++) {
            w->mass[found[k]] -= w->mass[found[k]] * fraction;
        }
    }
    c->o2_short = need > 0.0f ? 1.0f - take / need : 0.0f;
    c->o2_used += (double)take;

    // CO2 out, in proportion to the oxygen used: into CO2 near the mouth, or a new CO2 cell.
    float made = take * (CO2_KG_PER_S / O2_KG_PER_S);
    if (made > 0.0f && body_can_enter(w, mx, my) &&
        g_elem_behavior[world_elem(w, mx, my)] != BEHAVIOR_LIQUID) {
        int32_t co2[17];
        int nc = world_gas_region(w, mx, my, ELEM_CO2, mx, my, 4, 16, co2);
        size_t im = world_index(w, mx, my);
        if (w->cells[im].element == ELEM_CO2) {
            co2[nc++] = (int32_t)im;
        }
        bool done = false;
        if (nc > 0) {
            world_gas_spread_add(w, co2, nc, made, CREATURE_BODY_K);
            done = true;
        } else {
            // The mouth cell becomes CO2; the gas there (if any) is squeezed into its kind.
            ElementId g = w->cells[im].element;
            int32_t region[GAS_REGION_CELLS];
            int nr = 0;
            if (g != ELEM_VOID) {
                nr = world_gas_region(w, mx, my, g, mx, my, BREATH_RADIUS, BREATH_CELLS, region);
            }
            if (g == ELEM_VOID || nr > 0) {
                if (nr > 0) {
                    world_gas_spread_add(w, region, nr, w->mass[im], w->temp[im]);
                }
                world_set_gas(w, mx, my, ELEM_CO2, made, c->id * 2654435761u);
                w->temp[im] = CREATURE_BODY_K;
                done = true;
            }
        }
        if (done) {
            c->co2_made += (double)made;
        }
    }
    world_wake_rect(w, mx - BREATH_RADIUS, my - BREATH_RADIUS, mx + BREATH_RADIUS,
                    my + BREATH_RADIUS);
    world_wake_heat(w, mx, my, mx, my);
}

// Holds the body at 37 degC with at most the body's power.
static void body_heat(World *w, Creature *c, float work) {
    float budget = BODY_POWER_W * work * HEAT_TIME_SCALE_GAS * TICK_SECONDS; // J this tick
    float given = 0.0f;
    float cap = g_elem_cap[ELEM_BODY];
    for (int r = 0; r < CREATURE_H && given < budget; r++) {
        for (int k = 0; k < CREATURE_W && given < budget; k++) {
            size_t i = world_index(w, c->x + k, c->y + r);
            float need = (CREATURE_BODY_K - w->temp[i]) * cap;
            if (need > 0.0f) {
                float q = PR_MIN(need, budget - given);
                w->temp[i] += q / cap;
                given += q;
            }
        }
    }
    c->heat_w = given / (HEAT_TIME_SCALE_GAS * TICK_SECONDS);
    if (given > 0.0f) {
        world_wake_heat(w, c->x - 1, c->y - 1, c->x + CREATURE_W, c->y + CREATURE_H);
    }
}

static void update_one(World *w, Creature *c) {
    uint64_t h =
        pr_splitmix64(w->seed ^ (w->tick * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)c->id << 32));
    bool alive = c->state != CREATURE_DEAD;
    if (alive && c->wander && rand01(h) < 1.0f / 180.0f) {
        float u = rand01(pr_splitmix64(h));
        c->walk = (int8_t)(u < 0.4f ? -1 : (u < 0.8f ? 1 : 0)); // about every 3 s
    }
    if (!alive) {
        c->walk = 0;
    }
    if (c->walk != 0) {
        c->facing = c->walk;
    }
    physics(w, c);
    if (!alive) {
        return;
    }
    float work = c->walk != 0 && c->grounded ? WORK_WALKING : 1.0f;
    if (--c->breath_timer <= 0) {
        c->breath_timer = BREATH_TICKS;
        breathe(w, c, work);
    }
    body_heat(w, c, work);

    // Health from the air around the head (and whether the last breath found oxygen).
    bool danger = c->po2 < PO2_DANGER || c->pco2 > PCO2_DANGER || c->o2_short > 0.5f;
    float dt = TICK_SECONDS * c->metabolism_scale;
    if (danger) {
        c->health -= dt / DANGER_SECONDS;
        c->state = CREATURE_DANGER;
    } else {
        c->health = PR_MIN(c->health + dt / RECOVER_SECONDS, 1.0f);
        c->state = c->po2 < PO2_IMPAIRED ? CREATURE_IMPAIRED : CREATURE_OK;
    }
    if (c->health <= 0.0f) {
        c->health = 0.0f;
        c->state = CREATURE_DEAD;
        c->walk = 0;
    }
}

void creatures_update(World *w) {
    for (int i = 0; i < w->creature_count; i++) {
        update_one(w, &w->creatures[i]);
    }
}

int creature_spawn(World *w, int x, int y) {
    if (w->creature_count >= CREATURE_MAX) {
        return -1;
    }
    // The area must hold only void and gas; the gas must have somewhere to go: a cell of its kind
    // just outside the body.
    float mass[ELEM_COUNT] = {0};
    float heat[ELEM_COUNT] = {0};
    for (int r = 0; r < CREATURE_H; r++) {
        for (int k = 0; k < CREATURE_W; k++) {
            if (!world_in_bounds(w, x + k, y + r)) {
                return -1;
            }
            size_t i = world_index(w, x + k, y + r);
            ElementId e = w->cells[i].element;
            uint8_t b = g_elem_behavior[e];
            if (b != BEHAVIOR_NONE && b != BEHAVIOR_GAS) {
                return -1;
            }
            if (b == BEHAVIOR_GAS) {
                mass[e] += w->mass[i];
                heat[e] += w->mass[i] * w->temp[i];
            }
        }
    }
    // A start cell of each gas just outside (top and bottom rows, side columns).
    int sx[ELEM_COUNT], sy[ELEM_COUNT];
    for (int e = 0; e < ELEM_COUNT; e++) {
        sx[e] = -1;
    }
    for (int r = -1; r <= CREATURE_H; r++) {
        for (int k = -1; k <= CREATURE_W; k++) {
            bool edge = r == -1 || r == CREATURE_H || k == -1 || k == CREATURE_W;
            ElementId e = world_elem(w, x + k, y + r);
            if (edge && g_elem_behavior[e] == BEHAVIOR_GAS && sx[e] < 0) {
                sx[e] = x + k;
                sy[e] = y + r;
            }
        }
    }
    for (int e = 0; e < ELEM_COUNT; e++) {
        if (mass[e] > 0.0f && sx[e] < 0) {
            return -1;
        }
    }
    // Stamp the body, then put the gas it displaced around it.
    uint32_t id = w->creature_next_id++;
    for (int r = 0; r < CREATURE_H; r++) {
        for (int k = 0; k < CREATURE_W; k++) {
            world_set_cell(w, x + k, y + r, ELEM_BODY,
                           (uint32_t)(id * 7919u + (uint32_t)(r * 31 + k)));
        }
    }
    for (int e = 0; e < ELEM_COUNT; e++) {
        if (mass[e] <= 0.0f) {
            continue;
        }
        int32_t found[GAS_REGION_CELLS + 1];
        int n = world_gas_region(w, sx[e], sy[e], (ElementId)e, sx[e], sy[e], GAS_REGION_RADIUS,
                                 GAS_REGION_CELLS, found);
        found[n++] = (int32_t)world_index(w, sx[e], sy[e]);
        world_gas_spread_add(w, found, n, mass[e], heat[e] / mass[e]);
    }
    world_wake_rect(w, x - GAS_REGION_RADIUS, y - GAS_REGION_RADIUS,
                    x + CREATURE_W + GAS_REGION_RADIUS, y + CREATURE_H + GAS_REGION_RADIUS);
    world_wake_heat(w, x - 1, y - 1, x + CREATURE_W, y + CREATURE_H);

    Creature *c = &w->creatures[w->creature_count];
    *c = (Creature){0};
    c->x = x;
    c->y = y;
    c->facing = 1;
    c->id = id;
    c->health = 1.0f;
    c->metabolism_scale = 1.0f;
    c->breath_timer = BREATH_TICKS;
    int mx, my;
    mouth_cell(w, c, &mx, &my);
    c->po2 = c->pco2 = -1.0f; // not measured yet
    measure_air(w, c, mx, my);
    return w->creature_count++;
}
