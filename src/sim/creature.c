#include "sim/creature.h"

#include "core/rng.h"
#include "sim/enemy.h"
#include "sim/particle.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

// Steps up ledges up to STEP_UP cells (10 cm, for its 85 cm) without stopping.
#define STEP_UP  2
#define FALL_MAX 24.0f // cells per tick
// In liquid: drag per tick on the velocity, times the submerged fraction; swimming upward when
// the head is under (acceleration, cells per tick^2).
#define WATER_DRAG   0.15f
#define SWIM_ACCEL   0.03f
#define BODY_DENSITY 985.0f // kg/m^3 (floats, barely, in fresh water)
// Air control: fraction of walking speed gained per tick toward the wanted direction in the air
// (full speed in 10 ticks, 0.17 s; a jump lasts 0.64 s).
#define AIR_CONTROL 0.1f

// Breathing at rest (docs/SIMULATION.md -> Creatures): 14 breaths a minute of 0.5 L each (the
// tidal volume, TIDAL_CELLS cells of 0.125 L); 250 mL/min of O2 consumed and 200 mL/min of CO2
// produced (at 20 degC, 1 atm: 0.333 g and 0.368 g per minute). Walking multiplies breathing (its
// volume and gas exchange) and body heat by WORK_WALKING.
#define BREATH_TICKS  257 // 60 s / 14 breaths, in ticks
#define INHALE_TICKS  103 // breathing in takes 40% of a breath; it breathes out after it
#define TIDAL_CELLS   4.0f
#define O2_KG_PER_S   5.55e-6f
#define CO2_KG_PER_S  6.14e-6f
#define WORK_WALKING  3.0f
#define BREATH_RADIUS 16 // the gas a breath draws from, around the mouth
#define BREATH_CELLS  1024
// A breath in draws the same fraction of the gas of each of the DRAW_MIN gas cells nearest the
// mouth (more if the breath is bigger: each gives at most DRAW_MAX_FRAC of its gas), whatever
// the gas: it breathes in the air as it is mixed there (one gas per cell).
#define DRAW_MIN      32
#define DRAW_MAX_FRAC 0.5f
// The lungs take at most this fraction of the oxygen breathed in (at rest they take about a
// fifth: air goes in at 21% oxygen and out at about 16%).
#define O2_EXTRACT_MAX 0.5f
// Breathed out at 34 degC: the air in the lungs is warmed by the body (water vapour is left out:
// humidity is not modelled).
#define EXHALE_K 307.15f
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

// Void, gas or liquid (pushed aside); solids and powders stop the body (it stands on sand).
static inline bool body_can_enter(const World *w, int x, int y) {
    uint8_t b = g_elem_behavior[world_elem(w, x, y)];
    return b == BEHAVIOR_NONE || b >= BEHAVIOR_LIQUID;
}

// Powder in mid-air at (x, y): falling, or lying on a body. It is not on the ground, so it never
// blocks a body: a body moving into it lifts it out of the grid as a particle, which falls
// through the body (particle.c) and lands on the ground beside it.
static inline bool powder_in_air(const World *w, int x, int y) {
    if (!world_in_bounds(w, x, y)) {
        return false;
    }
    Cell c = w->cells[world_index(w, x, y)];
    if (g_elem_behavior[c.element] != BEHAVIOR_POWDER) {
        return false;
    }
    // Falling (the rest of a falling column with it), not into liquid (lifted there, it would
    // have nowhere to land); or lying on a body.
    ElementId below = world_elem(w, x, y + 1);
    return below == ELEM_BODY || ((c.aux & 0x7F) != 0 && g_elem_behavior[below] != BEHAVIOR_LIQUID);
}

// What a body can move into: what it pushes aside, and powder in mid-air.
static inline bool body_can_pass(const World *w, int x, int y) {
    return body_can_enter(w, x, y) || powder_in_air(w, x, y);
}

// Lifts the powder in mid-air at (x, y) out of the grid as a particle falling at its speed (the
// cell's aux holds it in 1/16 cells per tick) and sliding off the body at LIFT_SIDE cells per
// tick, toward the nearer side of creature c (so it lands beside the body, not under its feet).
// The cell becomes void. False if the particle list is full.
#define LIFT_SIDE 0.6f

static bool lift_powder(World *w, const Creature *c, int x, int y) {
    size_t i = world_index(w, x, y);
    Cell cell = w->cells[i];
    float side = 2 * (x - c->x) < c->w - 1 ? -LIFT_SIDE : LIFT_SIDE;
    Particle p = {(float)x + 0.5f, (float)y + 0.5f, side, (float)(cell.aux & 0x7F) / 16.0f, cell,
                  w->temp[i]};
    p.cell.aux = 0;
    if (!particle_add(w, &p)) {
        return false;
    }
    w->cells[i] = (Cell){ELEM_VOID, (uint8_t)(w->tick - 1), cell.shade, 0};
    w->temp[i] = 0.0f;
    gas_mix_clear(&w->gas[i]);
    world_note_moved(w, x, y);
    world_wake_rect(w, x - 1, y - 1, x + 1, y + 1);
    world_wake_heat(w, x - 1, y - 1, x + 1, y + 1);
    return true;
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
    GasMix g = w->gas[a];
    w->gas[a] = w->gas[b];
    w->gas[b] = g;
    world_note_moved(w, x0, y0);
    world_note_moved(w, x1, y1);
}

// Can the body move one cell by (dx, dy)? Every cell its leading edge moves into must be free of
// solids (void, gas or liquid, which it pushes, or powder in mid-air, which it lifts).
static bool can_move(const World *w, const Creature *c, int dx, int dy) {
    int n = dx != 0 ? c->h : c->w;
    for (int k = 0; k < n; k++) {
        int x = dx != 0 ? (dx > 0 ? c->x + c->w : c->x - 1) : c->x + k;
        int y = dx != 0 ? c->y + k : (dy > 0 ? c->y + c->h : c->y - 1);
        if (!body_can_pass(w, x, y)) {
            return false;
        }
    }
    return true;
}

// Matter classes for flowing around a body: 0 void or gas, 1 liquid, 2 anything else.
static inline int fluid_class(ElementId e) {
    uint8_t b = g_elem_behavior[e];
    return b == BEHAVIOR_NONE || b == BEHAVIOR_GAS ? 0 : (b == BEHAVIOR_LIQUID ? 1 : 2);
}

// Moving one cell up or down (dy), the body's column x trades its leading cell L (at ly) with its
// trailing cell T (at ty). If what lies beyond the trailing end (B) is not the same kind of fluid
// as L (landing in a pool: water under the feet, air above the head; or jumping out of one; B
// is the fluid beside T if T is against a wall), L does not go through the body to T: the fluid
// flows around it. In a column beside the body
// (within FLOW_SIDE columns), from the trailing end on, the first cell of L's kind (I) and the one
// before it (J, of B's kind) are the interface (the pool's surface): L takes J's place and J fills
// T, so the surface there rises (or falls) by one cell. Of those columns, the one whose surface is
// furthest toward the leading end is used (the lowest surface when the body sinks in, the highest
// when it rises out), nearest first: the surface stays level. Returns false (plain trade) if there
// is no such interface within the body's length.
#define FLOW_SIDE 8

// The interface row J in column sx (see flow_around), scanning from row `start` to the leading
// row ly, or INT32_MIN if none.
static int flow_interface(const World *w, int sx, ElementId l, ElementId b, int ly, int start,
                          int dy) {
    int cl = fluid_class(l), cb = fluid_class(b);
    for (int y = start; y != ly + dy; y += dy) {
        ElementId e = world_elem(w, sx, y);
        int ce = fluid_class(e);
        if (ce == 2) {
            return INT32_MIN; // a wall in the way
        }
        if (ce != cl) {
            continue;
        }
        // The first cell of L's kind: J is the one before it, of B's kind (and the same liquid
        // as L or B, where it is a liquid).
        int jy = y - dy;
        ElementId j = world_elem(w, sx, jy);
        if (y == start || fluid_class(j) != cb || (cl == 1 && e != l) || (cb == 1 && j != b)) {
            return INT32_MIN;
        }
        return jy;
    }
    return INT32_MIN;
}

static bool flow_around(World *w, const Creature *c, int x, int ly, int ty, int dy) {
    bool left_first = x - c->x < c->w / 2;
    ElementId l = world_elem(w, x, ly), b = world_elem(w, x, ty - dy);
    int start = ty - dy;
    if (fluid_class(b) == 2) {
        // The trailing end is against a wall (feet on the floor): T is filled by the fluid beside
        // it, as in the gap a foot leaves on the bottom of a pool.
        ElementId near = world_elem(w, left_first ? c->x - 1 : c->x + c->w, ty);
        ElementId far = world_elem(w, left_first ? c->x + c->w : c->x - 1, ty);
        b = fluid_class(near) != 2 ? near : far;
        start = ty;
    }
    int cl = fluid_class(l), cb = fluid_class(b);
    if (cl == 2 || cb == 2 || cl == cb) {
        return false;
    }
    int best_x = 0, best_y = INT32_MIN;
    bool open[2] = {true, true}; // no wall between the body and the column, on that side
    for (int d = 0; d <= FLOW_SIDE; d++) {
        for (int s = 0; s < 2; s++) {
            int side = (s == 0) == left_first ? 0 : 1;
            int sx = side == 0 ? c->x - 1 - d : c->x + c->w + d;
            if (!open[side]) {
                continue;
            }
            int jy = flow_interface(w, sx, l, b, ly, start, dy);
            if (jy == INT32_MIN) {
                // Keep looking further out only past fluid (a wall here closes this side).
                open[side] = fluid_class(world_elem(w, sx, ly)) != 2 ||
                             fluid_class(world_elem(w, sx, start)) != 2;
                continue;
            }
            if (best_y == INT32_MIN || jy * dy > best_y * dy) {
                best_x = sx;
                best_y = jy;
            }
        }
    }
    if (best_y == INT32_MIN) {
        return false;
    }
    swap_world_cells(w, x, ly, x, ty);          // the body into L; L to T
    swap_world_cells(w, x, ty, best_x, best_y); // L to J; J to T
    return true;
}

// Gas currents a moving body makes (docs/SIMULATION.md -> Gas currents): the air it pushes in
// front of it, and the wake it drags behind it.
#define BODY_PUSH_WIND 12
#define BODY_WAKE_WIND 18
// A breath out blows a puff from the mouth.
#define BREATH_WIND 14

static void push_wind(World *w, int x, int y, int dir, int speed) {
    if (world_in_bounds(w, x, y)) {
        gas_push_wind(&w->cells[world_index(w, x, y)], dir, speed);
    }
}

// Small piles at a walker's feet (docs/SIMULATION.md -> Creatures): powder (casings, spent
// bullets, loose soil, rubble, sand) in the lowest KICK_ROWS rows (20 cm) of the column it walks
// into is kicked ahead instead of stopping it, if nothing else is in the way and it is not the foot
// of a bigger heap (then the walker climbs). Each cell flies as a particle from just ahead, at
// twice the walker's speed and upward (well clear of it), and lands again.
#define KICK_ROWS  4
#define KICK_AHEAD 6

static float kick_rand(uint64_t *h) {
    *h = pr_splitmix64(*h);
    return (float)(*h >> 40) * (1.0f / 16777216.0f);
}

// The cell a kicked cell from row y of the column at lx flies from (dx: direction): the first
// that passes, going up from just ahead, or else from the column itself (above the pile; the
// walker is about to step into it, but powders in flight pass through bodies). Returns false if
// none within KICK_ROWS + 2 cells up.
static bool kick_from(const World *w, int lx, int y, int dx, int *fx, int *fy) {
    for (int k = 0; k < 2; k++) {
        int x = k == 0 ? lx + dx : lx;
        for (int up = 0; up <= KICK_ROWS + 2; up++) {
            if (particle_passes(w, x, y - up)) {
                *fx = x;
                *fy = y - up;
                return true;
            }
        }
    }
    return false;
}

static bool kick(World *w, Creature *c, int dx) {
    int lx = dx > 0 ? c->x + c->w : c->x - 1;
    int n = 0;
    for (int r = 0; r < c->h; r++) {
        int y = c->y + r, fx, fy;
        if (body_can_pass(w, lx, y)) {
            continue;
        }
        if (r < c->h - KICK_ROWS || g_elem_behavior[world_elem(w, lx, y)] != BEHAVIOR_POWDER ||
            !kick_from(w, lx, y, dx, &fx, &fy)) {
            return false; // a wall, a big heap, or nowhere to kick it
        }
        n++;
    }
    if (n == 0 || w->particle_count + n > PARTICLE_MAX) {
        return false;
    }
    // Not the foot of a heap: if the powder ahead reaches above the kick rows within KICK_AHEAD
    // columns, the walker climbs the slope instead (kicking its foot away would dig it into the
    // heap until the face is too high to kick or climb).
    int above = c->y + c->h - 1 - KICK_ROWS;
    for (int k = 0; k <= KICK_AHEAD; k++) {
        if (g_elem_behavior[world_elem(w, lx + dx * k, above)] == BEHAVIOR_POWDER &&
            !powder_in_air(w, lx + dx * k, above)) {
            return false;
        }
    }
    uint64_t h = pr_splitmix64(w->seed ^ pr_splitmix64(w->tick) ^ ((uint64_t)c->id << 48));
    for (int r = c->h - KICK_ROWS; r < c->h; r++) {
        int y = c->y + r, fx = lx + dx, fy = y;
        if (body_can_pass(w, lx, y)) {
            continue;
        }
        kick_from(w, lx, y, dx, &fx, &fy);
        size_t i = world_index(w, lx, y);
        Cell cell = w->cells[i];
        cell.aux = 0;
        Particle p = {(float)fx + 0.5f,
                      (float)fy + 0.5f,
                      (float)dx * (c->speed * 2.0f + 0.5f * kick_rand(&h)),
                      -(0.5f + 0.6f * kick_rand(&h)),
                      cell,
                      w->temp[i]};
        particle_add(w, &p);
        w->cells[i] = (Cell){ELEM_VOID, (uint8_t)(w->tick - 1), cell.shade, 0};
        w->temp[i] = 0.0f;
        gas_mix_clear(&w->gas[i]);
        world_note_moved(w, lx, y);
    }
    world_wake_rect(w, lx - 1, c->y + c->h - KICK_ROWS - 1, lx + 1, c->y + c->h);
    world_wake_heat(w, lx - 1, c->y + c->h - KICK_ROWS - 1, lx + 1, c->y + c->h);
    return true;
}

// Moves the body one cell. The matter in each cell it moves into trades places with the body cell
// it leaves at the other end: gas and liquid flow around the body, and nothing is lost. The air in
// front is pushed along and the air behind follows (gas currents). Walking, it kicks small piles
// at its feet out of the way.
static bool move(World *w, Creature *c, int dx, int dy) {
    if (!can_move(w, c, dx, dy) && !(dx != 0 && kick(w, c, dx) && can_move(w, c, dx, dy))) {
        return false;
    }
    // Powder in mid-air in the way is lifted out (it falls through the body).
    for (int k = 0, n = dx != 0 ? c->h : c->w; k < n; k++) {
        int x = dx != 0 ? (dx > 0 ? c->x + c->w : c->x - 1) : c->x + k;
        int y = dx != 0 ? c->y + k : (dy > 0 ? c->y + c->h : c->y - 1);
        if (!body_can_enter(w, x, y) && !lift_powder(w, c, x, y)) {
            return false; // the particle list is full
        }
    }
    int dir = gas_wind_dir_of(dx, dy);
    if (dx != 0) {
        int lx = dx > 0 ? c->x + c->w : c->x - 1;
        int tx = dx > 0 ? c->x : c->x + c->w - 1;
        (void)lx;
        (void)tx;
        for (int r = 0; r < c->h; r++) {
            // The row's runs of body cells (a monster that has lost pixels has holes): each run
            // trades its trailing end with the cell beyond its leading end.
            int y = c->y + r;
            for (int k = 0; k < c->w;) {
                if (world_elem(w, c->x + k, y) != ELEM_BODY) {
                    k++;
                    continue;
                }
                int a = k;
                while (k < c->w && world_elem(w, c->x + k, y) == ELEM_BODY) {
                    k++;
                }
                int b = k - 1; // the run is a..b
                int lead = dx > 0 ? c->x + b + 1 : c->x + a - 1, trail = dx > 0 ? c->x + a : c->x + b;
                swap_world_cells(w, lead, y, trail, y);
                push_wind(w, trail, y, dir, BODY_WAKE_WIND);
                push_wind(w, lead + dx, y, dir, BODY_PUSH_WIND);
            }
        }
        c->x += dx;
    } else {
        int ly = dy > 0 ? c->y + c->h : c->y - 1;
        int ty = dy > 0 ? c->y : c->y + c->h - 1;
        for (int k = 0; k < c->w; k++) {
            int x = c->x + k;
            // The column's runs of body cells: as above. A whole column may flow around the body.
            bool whole = true;
            for (int r = 0; r < c->h && whole; r++) {
                whole = world_elem(w, x, c->y + r) == ELEM_BODY;
            }
            if (whole) {
                if (!flow_around(w, c, x, ly, ty, dy)) {
                    swap_world_cells(w, x, ly, x, ty);
                }
                push_wind(w, x, ty, dir, BODY_WAKE_WIND);
                push_wind(w, x, ly + dy, dir, BODY_PUSH_WIND);
                continue;
            }
            for (int r = 0; r < c->h;) {
                if (world_elem(w, x, c->y + r) != ELEM_BODY) {
                    r++;
                    continue;
                }
                int a = r;
                while (r < c->h && world_elem(w, x, c->y + r) == ELEM_BODY) {
                    r++;
                }
                int b = r - 1;
                int lead = dy > 0 ? c->y + b + 1 : c->y + a - 1, trail = dy > 0 ? c->y + a : c->y + b;
                swap_world_cells(w, x, lead, x, trail);
                push_wind(w, x, trail, dir, BODY_WAKE_WIND);
                push_wind(w, x, lead + dy, dir, BODY_PUSH_WIND);
            }
        }
        c->y += dy;
    }
    world_wake_rect(w, c->x - 2, c->y - 2, c->x + c->w + 1, c->y + c->h + 1);
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
    for (int r = 0; r < c->h; r++) {
        n += g_elem_behavior[world_elem(w, c->x - 1, c->y + r)] == BEHAVIOR_LIQUID;
        n += g_elem_behavior[world_elem(w, c->x + c->w, c->y + r)] == BEHAVIOR_LIQUID;
    }
    return (float)n / (float)(2 * c->h);
}

static float rand01(uint64_t h) {
    return (float)(h >> 40) * (1.0f / 16777216.0f);
}

static void physics(World *w, Creature *c) {
    bool alive = c->state != CREATURE_DEAD;
    bool flier = alive && c->kind == KIND_FLYER; // its own thrust sets its velocity (enemy.c)
    c->submerged = submerged_fraction(w, c);
    float f = c->submerged;
    // Gravity, and buoyancy for the submerged part (water: 1000 kg/m^3).
    if (!flier) {
        c->vy += PARTICLE_GRAVITY * (1.0f - f * 1000.0f / BODY_DENSITY);
    }
    if (f > 0.0f) {
        c->vx *= 1.0f - WATER_DRAG * f;
        c->vy *= 1.0f - WATER_DRAG * f;
        int my = c->y + CREATURE_MOUTH;
        bool head_under = g_elem_behavior[world_elem(w, c->x - 1, my)] == BEHAVIOR_LIQUID &&
                          g_elem_behavior[world_elem(w, c->x + c->w, my)] == BEHAVIOR_LIQUID;
        if (alive && !flier && f > 0.5f && head_under) {
            c->vy -= SWIM_ACCEL; // head under: swim up
        }
    }
    if (alive && c->jump && c->grounded) {
        c->vy = -CREATURE_JUMP_SPEED;
        c->grounded = 0;
    }
    c->jump = 0;
    c->jet_hold = alive && c->thrust ? (uint8_t)PR_MIN(c->jet_hold + 1, 255) : 0;
    c->thrust = 0;
    if (alive && c->jet_hold >= JETPACK_DELAY && !c->grounded && c->vy > -JETPACK_MAX_RISE) {
        c->vy = PR_MAX(c->vy - JETPACK_ACCEL, -JETPACK_MAX_RISE);
    }
    c->vy = PR_CLAMP(c->vy, -FALL_MAX, FALL_MAX);
    float speed = c->speed * (c->state == CREATURE_IMPAIRED ? 0.5f : 1.0f);
    if (flier) {
        // velocity set by enemy_think
    } else if (alive && (c->grounded || f > 0.0f)) {
        c->vx = (float)c->walk * speed * (f > 0.0f ? 0.5f : 1.0f);
    } else if (alive) {
        // Air control (a game deviation: a real jumper cannot steer): in the air the walking
        // direction pulls the velocity toward walking speed, AIR_CONTROL of it per tick.
        float dv = (float)c->walk * speed - c->vx, max = AIR_CONTROL * speed;
        c->vx += PR_CLAMP(dv, -max, max);
    } else if (c->grounded) {
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
        c->fy = PR_MIN(c->fy, 0.0f); // resting: no fall left over (a jump takes off at once)
    }
}

// The cell the creature breathes from: in front of its face, else behind.
static void mouth_cell(const World *w, const Creature *c, int *mx, int *my) {
    *my = c->y + CREATURE_MOUTH;
    *mx = c->facing > 0 ? c->x + c->w : c->x - 1;
    if (!body_can_enter(w, *mx, *my)) {
        *mx = c->facing > 0 ? c->x - 1 : c->x + c->w;
    }
}

// Partial pressures of O2 and CO2 around the mouth: the mean over the cells gas could fill (void
// and gas) in the window.
static void measure_air(const World *w, Creature *c, int mx, int my) {
    double o2 = 0.0, co2 = 0.0;
    int n = 0;
    for (int y = my - HEAD_RADIUS; y <= my + HEAD_RADIUS; y++) {
        for (int x = mx - HEAD_RADIUS; x <= mx + HEAD_RADIUS; x++) {
            ElementId e = world_elem(w, x, y);
            if (e != ELEM_VOID && e != ELEM_GAS) {
                continue;
            }
            n++;
            if (e == ELEM_GAS) {
                o2 += (double)world_gas_partial_pressure(w, x, y, GAS_O2);
                co2 += (double)world_gas_partial_pressure(w, x, y, GAS_CO2);
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

// The gas cells (any gas) reachable from the mouth through gas and void, within BREATH_RADIUS,
// nearest first (breadth-first from the mouth cell, which comes first if it holds gas). Returns
// the count (at most BREATH_CELLS); none if the mouth is not in gas or void.
#define BREATH_SIDE (2 * BREATH_RADIUS + 1)

static int breath_region(const World *w, int mx, int my, int32_t *found) {
    if (!particle_passes(w, mx, my)) {
        return 0;
    }
    uint8_t seen[BREATH_SIDE][BREATH_SIDE];
    int8_t queue[BREATH_SIDE * BREATH_SIDE][2];
    memset(seen, 0, sizeof(seen));
    int head = 0, tail = 1, n = 0;
    queue[0][0] = queue[0][1] = 0;
    seen[BREATH_RADIUS][BREATH_RADIUS] = 1;
    if (world_elem(w, mx, my) != ELEM_VOID) {
        found[n++] = (int32_t)world_index(w, mx, my);
    }
    static const int8_t d4[4][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
    while (head < tail && n < BREATH_CELLS) {
        int ox = queue[head][0], oy = queue[head][1];
        head++;
        for (int k = 0; k < 4 && n < BREATH_CELLS; k++) {
            int dx = ox + d4[k][0], dy = oy + d4[k][1];
            if (dx < -BREATH_RADIUS || dx > BREATH_RADIUS || dy < -BREATH_RADIUS ||
                dy > BREATH_RADIUS || seen[dy + BREATH_RADIUS][dx + BREATH_RADIUS]) {
                continue;
            }
            seen[dy + BREATH_RADIUS][dx + BREATH_RADIUS] = 1;
            if (!particle_passes(w, mx + dx, my + dy)) {
                continue;
            }
            queue[tail][0] = (int8_t)dx;
            queue[tail][1] = (int8_t)dy;
            tail++;
            if (world_elem(w, mx + dx, my + dy) != ELEM_VOID) {
                found[n++] = (int32_t)world_index(w, mx + dx, my + dy);
            }
        }
    }
    return n;
}

// The volume of one breath in cells, and how many of the nearest gas cells it draws from.
static float breath_volume(const Creature *c, float work) {
    return TIDAL_CELLS * c->metabolism_scale * work;
}

static int breath_draw_cells(float volume, int available) {
    int n = PR_MAX(DRAW_MIN, (int)ceilf(volume / DRAW_MAX_FRAC));
    return PR_MIN(n, available);
}

// Breathing in: the lungs fill with a breath of the gas around the mouth (if it is in gas; they
// are not already full). Then, full or held, the body takes the oxygen it needs from the lungs (at
// most O2_EXTRACT_MAX of what they hold) and puts the CO2 it makes for it into them.
static void inhale(World *w, Creature *c, float work) {
    int mx, my;
    mouth_cell(w, c, &mx, &my);
    int32_t found[BREATH_CELLS];
    int n_all = c->holding ? 0 : breath_region(w, mx, my, found);
    if (n_all > 0) {
        stir(w, c, mx, my, work);
        measure_air(w, c, mx, my);
        n_all = breath_region(w, mx, my, found); // the stirring moved cells
        float volume = breath_volume(c, work);
        int n = breath_draw_cells(volume, n_all);
        float f = PR_MIN(volume / (float)n, DRAW_MAX_FRAC);
        for (int k = 0; k < n; k++) {
            GasMix part = w->gas[found[k]];
            gas_mix_scale(&part, f);
            gas_mix_add(&w->gas[found[k]], &part, -1.0f);
            gas_mix_add(&c->lungs, &part, 1.0f);
        }
        // The air flows in toward the mouth.
        int out = mx >= c->x + c->w ? 1 : -1;
        for (int k = -1; k <= 1; k++) {
            push_wind(w, mx + out, my + k, gas_wind_dir_of(-out, 0), BREATH_WIND / 2);
        }
        world_wake_rect(w, mx - BREATH_RADIUS, my - BREATH_RADIUS, mx + BREATH_RADIUS,
                        my + BREATH_RADIUS);
    }
    c->holding = 1; // until breathed out
    float seconds = (float)BREATH_TICKS * TICK_SECONDS * c->metabolism_scale * work;
    float need = O2_KG_PER_S * seconds;
    float take = PR_MIN(need, O2_EXTRACT_MAX * c->lungs.m[GAS_O2]);
    c->lungs.m[GAS_O2] -= take;
    float made = take * (CO2_KG_PER_S / O2_KG_PER_S);
    c->lungs.m[GAS_CO2] += made;
    c->o2_short = need > 0.0f ? 1.0f - take / need : 0.0f;
    c->o2_used += (double)take;
    c->co2_made += (double)made;
}

// Breathing out (if the mouth is in gas or void): the air in the lungs, at EXHALE_K, mixes into
// the nearest gas cells to the mouth (as many as it was drawn from), or fills the mouth cell if it
// is void; with a puff away from the face.
static void exhale(World *w, Creature *c, float work) {
    int mx, my;
    mouth_cell(w, c, &mx, &my);
    int32_t found[BREATH_CELLS];
    int n_all = breath_region(w, mx, my, found);
    if (n_all == 0 && !particle_passes(w, mx, my)) {
        return; // the mouth is in liquid: hold it
    }
    if (gas_total(&c->lungs) > 0.0f) {
        if (n_all > 0) {
            int n = breath_draw_cells(breath_volume(c, work), n_all);
            world_gas_spread_add(w, found, n, &c->lungs, EXHALE_K);
            for (int k = 0; k < n; k++) {
                world_note_moved(w, found[k] % w->width, found[k] / w->width);
            }
        } else {
            world_gas_emit(w, mx, my, &c->lungs, EXHALE_K, c->id * 2654435761u);
        }
        gas_mix_clear(&c->lungs);
    }
    c->holding = 0;
    // The breath out: a puff away from the face, rising a little (it is warm).
    int out = mx >= c->x + c->w ? 1 : -1;
    for (int k = -1; k <= 1; k++) {
        push_wind(w, mx, my + k, gas_wind_dir_of(out, 0), BREATH_WIND);
        push_wind(w, mx + out, my + k, gas_wind_dir_of(out, -1), BREATH_WIND - 4);
    }
    world_wake_rect(w, mx - BREATH_RADIUS, my - BREATH_RADIUS, mx + BREATH_RADIUS,
                    my + BREATH_RADIUS);
    world_wake_heat(w, mx - BREATH_RADIUS, my - BREATH_RADIUS, mx + BREATH_RADIUS,
                    my + BREATH_RADIUS);
}

// Holds the body at 37 degC with at most the body's power.
static void body_heat(World *w, Creature *c, float work) {
    float budget = BODY_POWER_W * work * HEAT_TIME_SCALE_GAS * TICK_SECONDS; // J this tick
    float given = 0.0f;
    float cap = g_elem_cap[ELEM_BODY];
    for (int r = 0; r < c->h && given < budget; r++) {
        for (int k = 0; k < c->w && given < budget; k++) {
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
        world_wake_heat(w, c->x - 1, c->y - 1, c->x + c->w, c->y + c->h);
    }
}

static void update_one(World *w, Creature *c) {
    uint64_t h =
        pr_splitmix64(w->seed ^ (w->tick * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)c->id << 32));
    bool alive = c->state != CREATURE_DEAD;
    if (c->hit_flash > 0) {
        c->hit_flash--;
    }
    if (alive && (c->kind == KIND_WALKER || c->kind == KIND_FLYER)) {
        enemy_think(w, c);
    }
    if (alive && c->wander && rand01(h) < 1.0f / 180.0f) {
        float u = rand01(pr_splitmix64(h));
        c->walk = (int8_t)(u < 0.4f ? -1 : (u < 0.8f ? 1 : 0)); // about every 3 s
    }
    if (!alive) {
        c->walk = 0;
    }
    if (c->walk != 0 && !c->aimed) {
        c->facing = c->walk;
    }
    physics(w, c);
    if (!alive) {
        if (c->bleed > 0.0f) {
            enemy_bleed(w, c);
        }
        return;
    }
    if (c->kind == KIND_WALKER || c->kind == KIND_FLYER) {
        return; // monsters do not breathe or give off heat
    }
    float work = c->walk != 0 && c->grounded ? WORK_WALKING : 1.0f;
    if (--c->breath_timer <= 0) {
        c->breath_timer = BREATH_TICKS;
        inhale(w, c, work);
    }
    if (c->holding && c->breath_timer <= BREATH_TICKS - INHALE_TICKS) {
        exhale(w, c, work);
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
        exhale(w, c, work); // the last breath out
    }
}

void creatures_update(World *w) {
    for (int i = 0; i < w->creature_count; i++) {
        Creature *c = &w->creatures[i];
        if (c->corpse) {
            continue; // now a rigid body
        }
        int cx = PR_CLAMP(c->x + c->w / 2, 0, w->width - 1) >> CHUNK_SHIFT;
        int cy = PR_CLAMP(c->y + c->h / 2, 0, w->height - 1) >> CHUNK_SHIFT;
        if (world_chunk(w, cx, cy)->active) { // outside the simulated area: frozen
            update_one(w, c);
        }
    }
}

int creature_spawn_sized(World *w, int x, int y, int cw, int ch) {
    if (w->creature_count >= CREATURE_MAX || cw < 1 || ch < 1 || cw > CREATURE_MAX_W ||
        ch > CREATURE_MAX_H) {
        return -1;
    }
    // The area must hold only void and gas; the gas must have somewhere to go: a gas cell just
    // outside the body.
    GasMix gas;
    gas_mix_clear(&gas);
    float heat = 0.0f;
    for (int r = 0; r < ch; r++) {
        for (int k = 0; k < cw; k++) {
            if (!world_in_bounds(w, x + k, y + r)) {
                return -1;
            }
            size_t i = world_index(w, x + k, y + r);
            ElementId e = w->cells[i].element;
            if (e != ELEM_VOID && e != ELEM_GAS) {
                return -1;
            }
            gas_mix_add(&gas, &w->gas[i], 1.0f);
            heat += gas_heat_cap(&w->gas[i]) * w->temp[i];
        }
    }
    // A start cell just outside (top and bottom rows, side columns).
    int sx = -1, sy = -1;
    for (int r = -1; r <= ch && sx < 0; r++) {
        for (int k = -1; k <= cw && sx < 0; k++) {
            bool edge = r == -1 || r == ch || k == -1 || k == cw;
            if (edge && world_elem(w, x + k, y + r) == ELEM_GAS) {
                sx = x + k;
                sy = y + r;
            }
        }
    }
    bool has_gas = gas_total(&gas) > 0.0f;
    if (has_gas && sx < 0) {
        return -1;
    }
    // Stamp the body, then put the gas it displaced around it.
    uint32_t id = w->creature_next_id++;
    for (int r = 0; r < ch; r++) {
        for (int k = 0; k < cw; k++) {
            world_set_cell(w, x + k, y + r, ELEM_BODY,
                           (uint32_t)(id * 7919u + (uint32_t)(r * 31 + k)));
        }
    }
    if (has_gas) {
        int32_t found[GAS_REGION_CELLS + 1];
        int n = world_gas_region(w, sx, sy, sx, sy, GAS_REGION_RADIUS, GAS_REGION_CELLS, found);
        found[n++] = (int32_t)world_index(w, sx, sy);
        world_gas_spread_add(w, found, n, &gas, heat / gas_heat_cap(&gas));
    }
    world_wake_rect(w, x - GAS_REGION_RADIUS, y - GAS_REGION_RADIUS, x + cw + GAS_REGION_RADIUS,
                    y + ch + GAS_REGION_RADIUS);
    world_wake_heat(w, x - 1, y - 1, x + cw, y + ch);

    Creature *c = &w->creatures[w->creature_count];
    *c = (Creature){0};
    c->x = x;
    c->y = y;
    c->w = (int16_t)cw;
    c->h = (int16_t)ch;
    c->facing = 1;
    c->id = id;
    c->health = 1.0f;
    c->hp = c->hp_max = PERSON_HP;
    c->metabolism_scale = 1.0f;
    c->speed = CREATURE_WALK_SPEED;
    c->breath_timer = BREATH_TICKS;
    int mx, my;
    mouth_cell(w, c, &mx, &my);
    c->po2 = c->pco2 = -1.0f; // not measured yet
    measure_air(w, c, mx, my);
    return w->creature_count++;
}

int creature_spawn(World *w, int x, int y) {
    return creature_spawn_sized(w, x, y, CREATURE_W, CREATURE_H);
}
