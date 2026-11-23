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
    uint8_t aux;   // per-behaviour scratch. Liquids: bit 7 = flow direction, bits 0-6 = fall speed
} Cell;

_Static_assert(sizeof(Cell) == 4, "Cell must stay 4 bytes");

// Dirty rect in chunk-local coordinates, inclusive. Empty when min_x > max_x.
typedef struct DirtyRect {
    int16_t min_x, min_y, max_x, max_y;
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
    uint8_t pad[2]; // 32 bytes total: chunk metadata never shares a cache line with 2+ others
} Chunk;

typedef struct World {
    int32_t width, height; // cells; multiples of CHUNK_SIZE
    int32_t chunks_x, chunks_y;
    Cell *cells;      // width * height, row-major
    float *temp;      // per cell, K; for phase-changing cells it also holds latent heat (elem_temp)
    float *temp_next; // heat pass scratch (Jacobi)
    float *mass;      // per cell, kg; gases only (0 elsewhere). Pressure: gas_pressure()
    uint8_t *piece;   // water pressure: body piece id of still liquid within its chunk (pressure.h)
    Chunk *chunks;
    uint64_t seed;
    uint64_t tick;
    JobPool *jobs;                  // not owned; NULL runs everything on the calling thread
    void *sim_scratch;              // owned; per-phase job state, allocated by the first sim_tick
    struct PressureState *pressure; // owned; water-pressure bodies (pressure.c)
    void *heat_scratch;             // owned; heat pass job state (heat.c)
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
}

static inline bool dirty_rect_empty(const DirtyRect *r) {
    return r->min_x > r->max_x;
}

static inline void dirty_rect_add(DirtyRect *r, int x0, int y0, int x1, int y1) {
    if (x0 < r->min_x) {
        r->min_x = (int16_t)x0;
    }
    if (y0 < r->min_y) {
        r->min_y = (int16_t)y0;
    }
    if (x1 > r->max_x) {
        r->max_x = (int16_t)x1;
    }
    if (y1 > r->max_y) {
        r->max_y = (int16_t)y1;
    }
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

// Puts a fresh cell of `e` at (x, y), with spawn temperature and random shade (a gas at 1 atm).
// Does not wake.
void world_set_cell(World *w, int x, int y, ElementId e, uint32_t random_bits);

// Same, for a gas cell holding mass m (kg).
void world_set_gas(World *w, int x, int y, ElementId e, float m, uint32_t random_bits);

// Pressure (Pa) of the gas at (x, y); 0 if it is not a gas.
float world_gas_pressure(const World *w, int x, int y);

// Gas regions: compressing or expanding gas at one spot (liquid moving into or out of it, one gas
// taking a cell from another) is spread over the gas of that kind around it, so no single cell
// jumps in pressure. world_gas_region() lists up to `max` (at most GAS_REGION_CELLS) cells of gas
// `g`, found by a breadth-first walk from (x, y) through gas of any kind (so it reaches the oxygen
// cells of mixed air) within `radius` (at most GAS_REGION_RADIUS) on each axis, skipping (x, y)
// and (ex, ey). Results are world indices, in a deterministic order.
#define GAS_REGION_RADIUS 16
#define GAS_REGION_CELLS  256
int world_gas_region(const World *w, int x, int y, ElementId g, int ex, int ey, int radius, int max,
                     int32_t *found);

// Adds mass m (kg, at stored temperature t) to the listed cells, an equal share each; their
// temperatures mix by mass.
void world_gas_spread_add(World *w, const int32_t *found, int n, float m, float t);

// Takes 1/(n+1) of each listed cell's mass (so all end like a new cell holding the sum); returns
// the mass taken, and its mean stored temperature in *t.
float world_gas_spread_take(World *w, const int32_t *found, int n, float *t);

// Gas pressure at (x, y) as a height of liquid `liquid` (liquid_head); 0 if not a gas.
static inline int32_t world_gas_head(const World *w, int x, int y, ElementId liquid) {
    return liquid_head(liquid, world_gas_pressure(w, x, y));
}
