#pragma once

#include "core/common.h"
#include "core/jobs.h"
#include "sim/elements.h"

#define CHUNK_SIZE  64
#define CHUNK_SHIFT 6
#define CHUNK_MASK  (CHUNK_SIZE - 1)
#define MAX_MOVE    (CHUNK_SIZE / 2)

_Static_assert(MAX_MOVE == MAX_MOVE_CELLS, "MAX_MOVE_CELLS must equal CHUNK_SIZE / 2");

typedef struct Cell {
    ElementId element;
    uint8_t stamp; // low 8 bits of the tick in which the cell was last updated or moved
    uint8_t shade; // random at creation; colour variation; moves with the cell
    uint8_t aux;   // per-behaviour scratch. Liquids and powders: bit 7 = flow direction, bits 0-6 =
                   // fall speed. Gases: a current (gas_wind): bits 3-7 speed, bits 0-2 direction
} Cell;

_Static_assert(sizeof(Cell) == 4, "Cell must stay 4 bytes");

// What a gas cell holds (D-035): kg of each gas species (GAS_O2...). All zero in other cells.
typedef struct GasMix {
    float m[GAS_SPECIES];
} GasMix;

// Activity tiles: a chunk is 4 x 4 tiles of 16 x 16 cells. A dirty rect also keeps which tiles
// were woken (bit ty * 4 + tx), so the cells to process are those of the rect inside those tiles:
// a bullet's diagonal trail wakes a strip of tiles, not the whole chunk (docs/ARCHITECTURE.md ->
// Dirty rects).
#define TILE_SIZE   16
#define TILE_SHIFT  4
#define CHUNK_TILES (CHUNK_SIZE / TILE_SIZE) // per row

_Static_assert(CHUNK_TILES == 4, "the tile mask holds 4 x 4 tiles");

// Dirty rect in chunk-local coordinates, inclusive, and the tiles it was woken in. Empty when
// min_x > max_x.
typedef struct DirtyRect {
    int8_t min_x, min_y, max_x, max_y;
    uint16_t tiles;
} DirtyRect;

typedef struct Chunk {
    DirtyRect cur;  // cells to process in the running tick
    DirtyRect next; // cells woken for the following tick
    // Range of physical temperatures of the chunk's cells (lo > hi: none). Widened when cells
    // arrive, recomputed by the heat pass. Lets moving cells skip the heat pass when nothing
    // around differs in temperature.
    float temp_lo, temp_hi;
    uint8_t render_dirty;
    uint8_t head_dirty;       // relabel this chunk's still liquid for water pressure this tick
    uint8_t head_dirty_next;  // ... next tick
    uint8_t heat_active;      // compute heat conduction this tick
    uint8_t heat_active_next; // ... next tick
    uint8_t heat_check;       // cells arrived: check next tick whether temperatures differ
    uint16_t cost;  // cells the movement pass visited here the last time it ran (job order)
    uint8_t active; // simulated (world_simulate_near); else frozen, its wakes kept for later
    uint8_t pad[3]; // 32 bytes total: chunk metadata never shares a cache line with 2+ others
} Chunk;

_Static_assert(sizeof(Chunk) == 32, "Chunk must stay 32 bytes");

// A crate lying on the ground (M6b): a box, not cells. Walking on it picks it up (pickup.h).
typedef enum PickupKind {
    PICKUP_AMMO = 0,
    PICKUP_HEALTH = 1,
} PickupKind;

#define PICKUP_MAX 256

typedef struct Pickup {
    int32_t x, y;  // top-left cell
    int16_t w, h;  // size in cells
    uint8_t kind;  // PickupKind
    uint8_t taken; // picked up: no longer there
} Pickup;

typedef struct World {
    int32_t width, height; // cells; multiples of CHUNK_SIZE
    int32_t chunks_x, chunks_y;
    Cell *cells;      // width * height, row-major
    float *temp;      // per cell, K; for phase-changing cells it also holds latent heat (elem_temp)
    float *temp_next; // heat pass scratch (Jacobi)
    GasMix *gas;      // per cell; gas cells only (zero elsewhere). Pressure: gas_mix_pressure()
    // Per cell: a gas cell's density at 1 atm and its temperature (kg/m^3; 0 for other cells), for
    // buoyancy and the sleep checks. Recomputed at the start of every tick for the awake cells
    // (sim.c), so at most a tick old where cells changed.
    float *rho;
    uint8_t *piece; // water pressure: body piece id of still liquid within its chunk (pressure.h)
    Chunk *chunks;
    uint64_t seed;
    uint64_t tick;
    JobPool *jobs;                  // not owned; NULL runs everything on the calling thread
    void *sim_scratch;              // owned; per-phase job state, allocated by the first sim_tick
    struct PressureState *pressure; // owned; water-pressure bodies (pressure.c)
    void *heat_scratch;             // owned; heat pass job state (heat.c)
    struct Particle *particles;     // owned; free particles in flight (particle.h)
    int32_t particle_count;
    uint32_t launched_by_pressure; // jets launched this tick (for SimStats)
    struct Creature *creatures;    // owned; creatures (creature.h)
    int32_t creature_count;
    uint32_t creature_next_id;
    struct Pickup *pickups; // owned; crates (PICKUP_MAX)
    int32_t pickup_count;
    // The size of the screen in cells (the game sets it each tick from the window and zoom): a
    // monster notices the player when it sees it on the screen and chases it until it is off it.
    int32_t view_w, view_h;
    struct PathField *paths; // owned; the monsters' routes toward the player (path.h)
} World;

// Width and height are rounded up to multiples of CHUNK_SIZE. Returns NULL on failure.
World *world_create(int32_t width, int32_t height, uint64_t seed);
void world_destroy(World *w);

// Sets every cell to void and wakes the whole world.
void world_clear(World *w);

// Runs the simulation (and rendering conversion) on this pool. NULL: single-threaded. Results are
// identical for any number of threads. The pool must outlive its use by the world.
static inline void world_set_job_pool(World *w, JobPool *pool) {
    w->jobs = pool;
}

// Gas currents (M5+): a gas cell's aux holds a direction (0 up, then clockwise in 45 degree steps)
// and a speed 0..GAS_WIND_MAX (it moves one cell with probability speed / 32 per tick).
#define GAS_WIND_MAX 31
static const int8_t k_wind_dir[8][2] = {{0, -1}, {1, -1}, {1, 0},  {1, 1},
                                        {0, 1},  {-1, 1}, {-1, 0}, {-1, -1}};
static inline int gas_wind_speed(uint8_t aux) {
    return aux >> 3;
}
static inline int gas_wind_dir(uint8_t aux) {
    return aux & 7;
}
static inline uint8_t gas_wind(int dir, int speed) {
    return (uint8_t)((PR_CLAMP(speed, 0, GAS_WIND_MAX) << 3) | (dir & 7));
}
// The direction index of a step (dx, dy), each -1..1, not both 0.
static inline int gas_wind_dir_of(int dx, int dy) {
    static const int8_t k_index[3][3] = {{7, 0, 1}, {6, 0, 2}, {5, 4, 3}};
    return k_index[dy + 1][dx + 1];
}
// Gives the gas cell at (x, y) a current, unless it already has a stronger one. Returns true if
// it changed (the caller wakes it).
static inline bool gas_push_wind(Cell *c, int dir, int speed) {
    if (g_elem_behavior[c->element] != BEHAVIOR_GAS || gas_wind_speed(c->aux) >= speed) {
        return false;
    }
    c->aux = gas_wind(dir, speed);
    return true;
}

// ---- Gas mixtures (D-035) ----

// Turbulence: a gas current (speed 0..GAS_WIND_MAX) mixes heat and gas faster, by 1 +
// GAS_TURB_PER_WIND * speed: an eddy diffusivity of u * l (the current's speed, 2.9 m/s at full
// speed, times one cell) is about 700 times air's sped-up thermal diffusivity at full speed
// (heat.c; D-034), 23 per unit of speed. Gas mixing uses the same factor (sim.c).
#define GAS_TURB_PER_WIND 23.0f

// Moles of gas in a mixture.
static inline float gas_moles(const GasMix *g) {
    float n = 0.0f;
    for (int s = 0; s < GAS_SPECIES; s++) {
        n += g->m[s] * g_species_inv_molar[s];
    }
    return n;
}

// Total mass, kg.
static inline float gas_total(const GasMix *g) {
    float m = 0.0f;
    for (int s = 0; s < GAS_SPECIES; s++) {
        m += g->m[s];
    }
    return m;
}

// Heat capacity, J/K.
static inline float gas_heat_cap(const GasMix *g) {
    float c = 0.0f;
    for (int s = 0; s < GAS_SPECIES; s++) {
        c += g->m[s] * g_species_cp[s];
    }
    return c;
}

// Pressure (Pa) of a cell of this mixture at temperature t (K).
static inline float gas_mix_pressure(const GasMix *g, float t) {
    return gas_pressure_moles(gas_moles(g), t);
}

// Density (kg/m^3) the mixture would have at 1 atm and temperature t: what buoyancy compares.
static inline float gas_mix_density_atm(const GasMix *g, float t) {
    float n = gas_moles(g);
    return n > 0.0f ? gas_total(g) / n * (P_ATM / GAS_R) / PR_MAX(t, 1.0f) : 0.0f;
}

static inline void gas_mix_clear(GasMix *g) {
    for (int s = 0; s < GAS_SPECIES; s++) {
        g->m[s] = 0.0f;
    }
}

static inline void gas_mix_scale(GasMix *g, float f) {
    for (int s = 0; s < GAS_SPECIES; s++) {
        g->m[s] *= f;
    }
}

// d += f * a.
static inline void gas_mix_add(GasMix *d, const GasMix *a, float f) {
    for (int s = 0; s < GAS_SPECIES; s++) {
        d->m[s] += a->m[s] * f;
    }
}

// Air (AIR_O2_FRAC, AIR_CO2_FRAC, the rest nitrogen) at pressure p and temperature t, one cell.
GasMix gas_mix_air(float p, float t);

// Species s alone, m kg.
static inline GasMix gas_mix_pure(int s, float m) {
    GasMix g;
    gas_mix_clear(&g);
    g.m[s] = m;
    return g;
}

// Still liquid: liquid that is not falling. Only still liquid carries pressure.
static inline bool cell_is_still_liquid(Cell c) {
    return g_elem_behavior[c.element] == BEHAVIOR_LIQUID && (c.aux & 0x7F) == 0;
}

static inline bool world_in_bounds(const World *w, int x, int y);

// Is the still liquid `e` at (x, y) a free surface? True if what is above it could be pushed
// aside: void, a lighter fluid, or the same liquid falling (a stream is not part of the pool).
static inline bool liquid_surface_free(const World *w, int x, int y, ElementId e);

static inline bool world_in_bounds(const World *w, int x, int y) {
    return (unsigned)x < (unsigned)w->width && (unsigned)y < (unsigned)w->height;
}

static inline size_t world_index(const World *w, int x, int y) {
    return (size_t)y * (size_t)w->width + (size_t)x;
}

static inline ElementId world_elem(const World *w, int x, int y) {
    return world_in_bounds(w, x, y) ? w->cells[world_index(w, x, y)].element : ELEM_OUT;
}

static inline Chunk *world_chunk(World *w, int cx, int cy) {
    return &w->chunks[cy * w->chunks_x + cx];
}

static inline void dirty_rect_clear(DirtyRect *r) {
    r->min_x = r->min_y = CHUNK_SIZE;
    r->max_x = r->max_y = -1;
    r->tiles = 0;
}

static inline bool dirty_rect_empty(const DirtyRect *r) {
    return r->min_x > r->max_x;
}

// The tiles overlapping the chunk-local rect (x0, y0)-(x1, y1), inclusive, within the chunk.
static inline uint16_t tile_mask(int x0, int y0, int x1, int y1) {
    int tx0 = x0 >> TILE_SHIFT, tx1 = x1 >> TILE_SHIFT;
    int ty0 = y0 >> TILE_SHIFT, ty1 = y1 >> TILE_SHIFT;
    unsigned cols = (0xFu << tx0) & (0xFu >> (3 - tx1));                   // bits tx0..tx1
    unsigned rows = (0x1111u << (4 * ty0)) & (0x1111u >> (4 * (3 - ty1))); // bit 0 of each row
    return (uint16_t)(cols * rows); // no carries: cols < 16 and rows' bits are 4 apart
}

static inline void dirty_rect_add(DirtyRect *r, int x0, int y0, int x1, int y1) {
    PR_ASSERT(x0 >= 0 && y0 >= 0 && x1 < CHUNK_SIZE && y1 < CHUNK_SIZE && x0 <= x1 && y0 <= y1);
    if (x0 < r->min_x) {
        r->min_x = (int8_t)x0;
    }
    if (y0 < r->min_y) {
        r->min_y = (int8_t)y0;
    }
    if (x1 > r->max_x) {
        r->max_x = (int8_t)x1;
    }
    if (y1 > r->max_y) {
        r->max_y = (int8_t)y1;
    }
    r->tiles |= tile_mask(x0, y0, x1, y1);
}

// d becomes the union of d and s (bounding rect, tiles OR-ed): order-independent.
static inline void dirty_rect_merge(DirtyRect *d, const DirtyRect *s) {
    if (dirty_rect_empty(s)) {
        return;
    }
    d->min_x = PR_MIN(d->min_x, s->min_x);
    d->min_y = PR_MIN(d->min_y, s->min_y);
    d->max_x = PR_MAX(d->max_x, s->max_x);
    d->max_y = PR_MAX(d->max_y, s->max_y);
    d->tiles |= s->tiles;
}

// The column spans of the rect r inside its woken tiles, on tile row ty: up to 2 runs of
// adjacent tiles, clipped to the rect's columns, left to right, as chunk-local [x0[k], x1[k]].
// Returns the number of spans.
static inline int dirty_rect_spans(const DirtyRect *r, int ty, int x0[2], int x1[2]) {
    unsigned bits = (r->tiles >> (4 * ty)) & 0xFu;
    int n = 0;
    for (int tx = 0; tx < CHUNK_TILES && bits;) {
        if (!(bits & (1u << tx))) {
            tx++;
            continue;
        }
        int t0 = tx;
        while (tx < CHUNK_TILES && (bits & (1u << tx))) {
            tx++;
        }
        int a = PR_MAX(t0 * TILE_SIZE, r->min_x), b = PR_MIN(tx * TILE_SIZE - 1, r->max_x);
        if (a <= b) {
            PR_ASSERT(n < 2);
            x0[n] = a;
            x1[n] = b;
            n++;
        }
    }
    return n;
}

// Simulated area (D-040): the world is cut into regions of SIM_REGION x SIM_REGION cells (8 x 8
// chunks), and only the 3 x 3 regions around the player are simulated. Outside, time stops: cells,
// heat and creatures stay as they are, and what reaches them (matter, wakes) waits there until they
// are simulated again. Free particles keep flying everywhere.
#define SIM_REGION 512

_Static_assert(SIM_REGION % CHUNK_SIZE == 0, "regions are whole chunks");

// Simulates the 3 x 3 regions around the cell (x, y) only.
void world_simulate_near(World *w, int x, int y);

// Simulates the whole world (the default: tests, scenes without a player).
void world_simulate_all(World *w);

// Wakes the chunk-local rect of chunk c for this tick if it is simulated, else for when it is
// (heat and water pressure, which run before the movement pass).
static inline void chunk_wake_now(Chunk *c, int x0, int y0, int x1, int y1) {
    dirty_rect_add(c->active ? &c->cur : &c->next, x0, y0, x1, y1);
}

// Wake every cell in the world-space rect (inclusive, clamped) for the next tick, and mark the
// chunks it touches for re-render and water-pressure relabelling.
void world_wake_rect(World *w, int x0, int y0, int x1, int y1);

// Temperatures or materials changed in the rect (edits, scenes): run the heat pass there next
// tick.
void world_wake_heat(World *w, int x0, int y0, int x1, int y1);

// A cell was moved to (x, y) outside the movement pass (water-pressure transfers): record its
// temperature so the heat pass runs next tick if it differs from its surroundings.
void world_note_moved(World *w, int x, int y);

static inline bool liquid_surface_free(const World *w, int x, int y, ElementId e) {
    if (y <= 0) {
        return false;
    }
    Cell a = w->cells[world_index(w, x, y - 1)];
    uint8_t b = g_elem_behavior[a.element];
    if (b == BEHAVIOR_NONE) {
        return true;
    }
    if (b >= BEHAVIOR_LIQUID && g_elem_density_q[a.element] < g_elem_density_q[e]) {
        return true;
    }
    return a.element == e && !cell_is_still_liquid(a);
}

// Puts a fresh cell of `e` at (x, y), with spawn temperature and random shade. A gas species
// (ELEM_OXYGEN...) makes a gas cell of that species alone at 1 atm; ELEM_GAS, air at 1 atm.
// Does not wake.
void world_set_cell(World *w, int x, int y, ElementId e, uint32_t random_bits);

// A gas cell of species element e alone, holding mass m (kg), at e's spawn temperature.
void world_set_gas(World *w, int x, int y, ElementId e, float m, uint32_t random_bits);

// A gas cell holding the mixture g at stored temperature t.
void world_set_mix(World *w, int x, int y, const GasMix *g, float t, uint32_t random_bits);

// Pressure (Pa) of the gas at (x, y); 0 if it is not a gas.
float world_gas_pressure(const World *w, int x, int y);

// Partial pressure (Pa) of gas species s at (x, y); 0 if it is not a gas.
float world_gas_partial_pressure(const World *w, int x, int y, int s);

// Gas regions: compressing or expanding gas at one spot (liquid moving into or out of it, a
// particle landing in it) is spread over the gas around it, so no single cell jumps in pressure.
// world_gas_region() lists up to `max` (at most GAS_REGION_CELLS) gas cells, found by a
// breadth-first walk from (x, y) through gas within `radius` (at most GAS_REGION_RADIUS) on each
// axis, skipping (x, y) and (ex, ey). Results are world indices, in a deterministic order.
#define GAS_REGION_RADIUS 16
#define GAS_REGION_CELLS  256
int world_gas_region(const World *w, int x, int y, int ex, int ey, int radius, int max,
                     int32_t *found);

// Adds the mixture g (at stored temperature t) to the listed cells, an equal share each; their
// temperatures mix by heat capacity (energy is conserved).
void world_gas_spread_add(World *w, const int32_t *found, int n, const GasMix *g, float t);

// Takes 1/(n+1) of each listed cell's gas (so all end like a new cell holding the sum) into
// *out; its mean temperature (by heat capacity) into *t.
void world_gas_spread_take(World *w, const int32_t *found, int n, GasMix *out, float *t);

// Mixes gas g (at stored temperature t) into the cell (x, y): added to its gas, temperatures
// mixing by heat capacity, or, in void, a new gas cell. Wakes the area. Returns false if the cell
// holds neither gas nor void. Serial use only (between ticks, or in creature or particle code).
bool world_gas_emit(World *w, int x, int y, const GasMix *g, float t, uint32_t random_bits);

// Adds energy-conserving: cell i's gas (heat capacity ci at temperature ti) receives gas of heat
// capacity ca at temperature ta. Returns the mixed temperature.
static inline float gas_mix_temp(float ci, float ti, float ca, float ta) {
    float c = ci + ca;
    return c > 0.0f ? (ci * ti + ca * ta) / c : ta;
}

// Gas pressure at (x, y) as a height of liquid `liquid` (liquid_head); 0 if not a gas.
static inline int32_t world_gas_head(const World *w, int x, int y, ElementId liquid) {
    return liquid_head(liquid, world_gas_pressure(w, x, y));
}
