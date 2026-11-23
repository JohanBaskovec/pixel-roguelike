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
    uint8_t render_dirty;
    uint8_t pad[15]; // 32 bytes total: chunk metadata never shares a cache line with 2+ others
} Chunk;

typedef struct World {
    int32_t width, height; // cells; multiples of CHUNK_SIZE
    int32_t chunks_x, chunks_y;
    Cell *cells; // width * height, row-major
    float *temp; // Kelvin, per cell (heat flow arrives in M3, with a second buffer)
    Chunk *chunks;
    uint64_t seed;
    uint64_t tick;
    JobPool *jobs;     // not owned; NULL runs everything on the calling thread
    void *sim_scratch; // owned; per-phase job state, allocated by the first sim_tick
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
// chunks it touches for re-render.
void world_wake_rect(World *w, int x0, int y0, int x1, int y1);

// Puts a fresh cell of `e` at (x, y), with spawn temperature and random shade. Does not wake.
void world_set_cell(World *w, int x, int y, ElementId e, uint32_t random_bits);
