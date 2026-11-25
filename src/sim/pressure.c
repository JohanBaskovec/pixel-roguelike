#include "sim/pressure.h"

#include "core/jobs.h"
#include "core/rng.h"
#include "sim/particle.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PIECES  255 // body pieces per chunk; further pieces get no pressure until relabelled
#define NODE_SHIFT  8   // node = chunk index << NODE_SHIFT | piece id
#define MAX_SOURCES 16  // pieces tried per transfer when looking for a top cell
#define SOURCE_ROWS 8   // rows below the body's top also searched once the top row is used up

typedef struct Piece {
    int32_t top;     // smallest head among this piece's free-surface cells, PRESSURE_NO_TOP if none
    int16_t top_row; // row of the surface cell with that head
    uint8_t element;
} Piece;

typedef struct ChunkPieces {
    uint16_t count;
    int32_t last_top;            // best body top among this chunk's pieces at the previous rebuild
    Piece piece[MAX_PIECES + 1]; // ids 1..count
} ChunkPieces;

typedef struct LabelJob {
    World *w;
    int chunk;
} LabelJob;

struct PressureState {
    int chunks;
    ChunkPieces *cp;
    LabelJob *jobs; // one per chunk
    // Per node (chunk << 8 | piece):
    int32_t *parent;   // union-find
    int32_t *root;     // root after the rebuild
    int32_t *src_next; // next node of the same body whose top is the body's top (list)
    // Per root node:
    int32_t *top;       // the body's top: smallest head of its free-surface cells
    int16_t *used;      // rows below the top already taken by transfers this tick
    uint64_t *gas_tick; // last tick the body compressed or expanded gas (GAS_LIQUID_INTERVAL)
    int32_t *src_head;  // first node of the list above
};

PressureState *pressure_create(const World *w) {
    PressureState *ps = calloc(1, sizeof(PressureState));
    if (!ps) {
        return NULL;
    }
    ps->chunks = w->chunks_x * w->chunks_y;
    size_t nodes = (size_t)ps->chunks << NODE_SHIFT;
    ps->cp = pr_aligned_alloc_zero(64, sizeof(ChunkPieces) * (size_t)ps->chunks);
    ps->jobs = pr_aligned_alloc_zero(64, sizeof(LabelJob) * (size_t)ps->chunks);
    ps->parent = pr_aligned_alloc_zero(64, sizeof(int32_t) * nodes);
    ps->root = pr_aligned_alloc_zero(64, sizeof(int32_t) * nodes);
    ps->src_next = pr_aligned_alloc_zero(64, sizeof(int32_t) * nodes);
    ps->top = pr_aligned_alloc_zero(64, sizeof(int32_t) * nodes);
    ps->used = pr_aligned_alloc_zero(64, sizeof(int16_t) * nodes);
    ps->gas_tick = pr_aligned_alloc_zero(64, sizeof(uint64_t) * nodes);
    ps->src_head = pr_aligned_alloc_zero(64, sizeof(int32_t) * nodes);
    if (!ps->cp || !ps->jobs || !ps->parent || !ps->root || !ps->src_next || !ps->top ||
        !ps->used || !ps->gas_tick || !ps->src_head) {
        pressure_destroy(ps);
        return NULL;
    }
    for (int i = 0; i < ps->chunks; i++) {
        ps->cp[i].last_top = PRESSURE_NO_TOP;
    }
    return ps;
}

void pressure_destroy(PressureState *ps) {
    if (!ps) {
        return;
    }
    pr_aligned_free(ps->cp);
    pr_aligned_free(ps->jobs);
    pr_aligned_free(ps->parent);
    pr_aligned_free(ps->root);
    pr_aligned_free(ps->src_next);
    pr_aligned_free(ps->top);
    pr_aligned_free(ps->used);
    pr_aligned_free(ps->gas_tick);
    pr_aligned_free(ps->src_head);
    free(ps);
}

// ---------------------------------------------------------------------------------------------
// Labelling: flood-fill each chunk's still liquid into pieces (4-connected, same element).
// ---------------------------------------------------------------------------------------------

static void label_chunk(void *user, int index) {
    LabelJob *job = &((LabelJob *)user)[index];
    World *w = job->w;
    ChunkPieces *cp = &w->pressure->cp[job->chunk];
    const int bx = (job->chunk % w->chunks_x) << CHUNK_SHIFT;
    const int by = (job->chunk / w->chunks_x) << CHUNK_SHIFT;

    uint8_t visited[CHUNK_SIZE][CHUNK_SIZE];
    uint16_t stack[CHUNK_SIZE * CHUNK_SIZE];
    memset(visited, 0, sizeof(visited));
    int count = 0;

    for (int ly = 0; ly < CHUNK_SIZE; ly++) {
        for (int lx = 0; lx < CHUNK_SIZE; lx++) {
            size_t i = world_index(w, bx + lx, by + ly);
            if (visited[ly][lx]) {
                continue;
            }
            Cell c = w->cells[i];
            if (!cell_is_still_liquid(c)) {
                w->piece[i] = 0;
                continue;
            }
            uint8_t e = c.element;
            int id = count < MAX_PIECES ? ++count : 0;
            int32_t top = PRESSURE_NO_TOP;
            int16_t top_row = PRESSURE_NO_TOP & INT16_MAX;
            int sp = 0;
            stack[sp++] = (uint16_t)(ly << CHUNK_SHIFT | lx);
            visited[ly][lx] = 1;
            while (sp > 0) {
                int v = stack[--sp];
                int vx = v & CHUNK_MASK, vy = v >> CHUNK_SHIFT;
                int x = bx + vx, y = by + vy;
                w->piece[world_index(w, x, y)] = (uint8_t)id;
                if (liquid_surface_free(w, x, y, e)) {
                    // Head: the row, raised by the pressure of the gas above (if any).
                    int32_t head = y - pressure_load_head(w, x, y - 1, e, PRESSURE_LOAD_SCAN);
                    if (head < top || (head == top && y < top_row)) {
                        top = head;
                        top_row = (int16_t)y;
                    }
                }
                static const int8_t d4[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (int k = 0; k < 4; k++) {
                    int nx = vx + d4[k][0], ny = vy + d4[k][1];
                    if (nx < 0 || ny < 0 || nx >= CHUNK_SIZE || ny >= CHUNK_SIZE ||
                        visited[ny][nx]) {
                        continue;
                    }
                    Cell n = w->cells[world_index(w, bx + nx, by + ny)];
                    if (n.element == e && cell_is_still_liquid(n)) {
                        visited[ny][nx] = 1;
                        stack[sp++] = (uint16_t)(ny << CHUNK_SHIFT | nx);
                    }
                }
            }
            if (id) {
                cp->piece[id] = (Piece){top, top_row, e};
            }
        }
    }
    cp->count = (uint16_t)count;
}

// ---------------------------------------------------------------------------------------------
// Bodies: union-find over all pieces, joined where still liquid touches across a chunk border.
// ---------------------------------------------------------------------------------------------

static int32_t uf_find(int32_t *parent, int32_t a) {
    while (parent[a] != a) {
        parent[a] = parent[parent[a]];
        a = parent[a];
    }
    return a;
}

static void uf_union(int32_t *parent, int32_t a, int32_t b) {
    a = uf_find(parent, a);
    b = uf_find(parent, b);
    if (a != b) {
        if (a < b) { // deterministic: the smaller node becomes the root
            parent[b] = a;
        } else {
            parent[a] = b;
        }
    }
}

static void join_border(World *w, int ca, int ax, int ay, int cb, int bx, int by, int dx, int dy) {
    PressureState *ps = w->pressure;
    for (int i = 0; i < CHUNK_SIZE; i++) {
        size_t ia = world_index(w, ax + dx * i, ay + dy * i);
        size_t ib = world_index(w, bx + dx * i, by + dy * i);
        uint8_t pa = w->piece[ia], pb = w->piece[ib];
        if (pa && pb && w->cells[ia].element == w->cells[ib].element) {
            uf_union(ps->parent, ca << NODE_SHIFT | pa, cb << NODE_SHIFT | pb);
        }
    }
}

static void rebuild_bodies(World *w) {
    PressureState *ps = w->pressure;
    for (int ch = 0; ch < ps->chunks; ch++) {
        for (int id = 1; id <= ps->cp[ch].count; id++) {
            ps->parent[ch << NODE_SHIFT | id] = ch << NODE_SHIFT | id;
        }
    }
    for (int cy = 0; cy < w->chunks_y; cy++) {
        for (int cx = 0; cx < w->chunks_x; cx++) {
            int ch = cy * w->chunks_x + cx;
            if (ps->cp[ch].count == 0) {
                continue;
            }
            int bx = cx << CHUNK_SHIFT, by = cy << CHUNK_SHIFT;
            if (cx + 1 < w->chunks_x && ps->cp[ch + 1].count) { // right border
                join_border(w, ch, bx + CHUNK_SIZE - 1, by, ch + 1, bx + CHUNK_SIZE, by, 0, 1);
            }
            if (cy + 1 < w->chunks_y && ps->cp[ch + w->chunks_x].count) { // bottom border
                join_border(w, ch, bx, by + CHUNK_SIZE - 1, ch + w->chunks_x, bx, by + CHUNK_SIZE,
                            1, 0);
            }
        }
    }
    // Roots, each body's top, and the list of pieces that reach it (in node order).
    for (int ch = 0; ch < ps->chunks; ch++) {
        for (int id = 1; id <= ps->cp[ch].count; id++) {
            int32_t node = ch << NODE_SHIFT | id;
            int32_t r = uf_find(ps->parent, node);
            ps->root[node] = r;
            ps->top[r] = PRESSURE_NO_TOP;
            ps->used[r] = 0;
            ps->src_head[r] = -1;
        }
    }
    for (int ch = 0; ch < ps->chunks; ch++) {
        for (int id = 1; id <= ps->cp[ch].count; id++) {
            int32_t r = ps->root[ch << NODE_SHIFT | id];
            int32_t t = ps->cp[ch].piece[id].top;
            if (t < ps->top[r]) {
                ps->top[r] = t;
            }
        }
    }
    for (int ch = ps->chunks - 1; ch >= 0; ch--) {
        for (int id = ps->cp[ch].count; id >= 1; id--) {
            int32_t node = ch << NODE_SHIFT | id;
            int32_t r = ps->root[node];
            if (ps->cp[ch].piece[id].top == ps->top[r] && ps->top[r] != PRESSURE_NO_TOP) {
                ps->src_next[node] = ps->src_head[r];
                ps->src_head[r] = node;
            }
        }
    }
}

void pressure_update(World *w) {
    PressureState *ps = w->pressure;
    // Label dirty chunks (in parallel: each job writes only its own chunk's labels).
    int n = 0;
    LabelJob *list = ps->jobs;
    for (int ch = 0; ch < ps->chunks; ch++) {
        if (w->chunks[ch].head_dirty) {
            w->chunks[ch].head_dirty = 0;
            list[n++] = (LabelJob){w, ch};
        }
    }
    if (n == 0) {
        return;
    }
    jobs_run(w->jobs, n, label_chunk, list);
    rebuild_bodies(w);

    // Chunks whose body top rose: their cells may now be pushed toward openings they are next
    // to. Simulate the whole chunk this tick.
    for (int ch = 0; ch < ps->chunks; ch++) {
        int32_t best = PRESSURE_NO_TOP;
        for (int id = 1; id <= ps->cp[ch].count; id++) {
            int32_t t = ps->top[ps->root[ch << NODE_SHIFT | id]];
            if (t < best) {
                best = t;
            }
        }
        if (best < ps->cp[ch].last_top) {
            chunk_wake_now(&w->chunks[ch], 0, 0, CHUNK_SIZE - 1, CHUNK_SIZE - 1);
        }
        ps->cp[ch].last_top = best;
    }
}

// A run of this many void or gas cells ends the column of liquid above a cell; shorter gaps are
// bubbles inside it (moving liquids leave them), and the liquid above them still weighs on what
// is below. Every liquid in the column counts, by its density (drops of water sinking through a
// column of oil weigh on it too).
#define LOAD_GAP 3

int32_t pressure_load_head(const World *w, int x, int y, ElementId liquid, int max_scan) {
    float rho = g_elem_defs[liquid].density;
    float h = 0.0f, gas_p = -1.0f;
    int gap = 0;
    for (int k = 0, yy = y; k < max_scan && yy >= 0; k++, yy--) {
        ElementId e = world_elem(w, x, yy);
        uint8_t b = g_elem_behavior[e];
        if (b == BEHAVIOR_NONE || b == BEHAVIOR_GAS) {
            if (gap++ == 0) {
                gas_p = world_gas_pressure(w, x, yy); // the gas over the column, if it ends here
            }
            if (gap >= LOAD_GAP) {
                break;
            }
            continue;
        }
        if (b != BEHAVIOR_LIQUID) {
            gas_p = gap > 0 ? gas_p : 0.0f; // a solid or powder closes the column
            break;
        }
        gap = 0;
        gas_p = -1.0f;
        h += g_elem_defs[e].density / rho;
    }
    if (gas_p > 0.0f) {
        h += gas_p / (rho * GRAVITY * CELL_SIZE_M);
    }
    return (int32_t)PR_MIN(h + 0.5f, 1e6f);
}

// The root node of the still cell at (x, y), or -1.
static int32_t body_root(const World *w, int x, int y) {
    uint8_t id = w->piece[world_index(w, x, y)];
    if (!id) {
        return -1;
    }
    int ch = (y >> CHUNK_SHIFT) * w->chunks_x + (x >> CHUNK_SHIFT);
    return w->pressure->root[ch << NODE_SHIFT | id];
}

int32_t pressure_body_top(const World *w, int x, int y) {
    int32_t r = body_root(w, x, y);
    if (r < 0 || w->pressure->top[r] == PRESSURE_NO_TOP) {
        return PRESSURE_NO_TOP;
    }
    return w->pressure->top[r] + w->pressure->used[r];
}

int32_t pressure_body_top_row(const World *w, int x, int y) {
    int32_t r = body_root(w, x, y);
    if (r < 0 || w->pressure->top[r] == PRESSURE_NO_TOP) {
        return PRESSURE_NO_TOP;
    }
    const PressureState *ps = w->pressure;
    int32_t node = ps->src_head[r];
    if (node < 0) {
        return PRESSURE_NO_TOP;
    }
    return ps->cp[node >> NODE_SHIFT].piece[node & ((1 << NODE_SHIFT) - 1)].top_row + ps->used[r];
}

// ---------------------------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------------------------

static inline bool open_for(const World *w, int x, int y, uint32_t density) {
    ElementId t = world_elem(w, x, y);
    uint8_t b = g_elem_behavior[t];
    return b == BEHAVIOR_NONE || (b >= BEHAVIOR_LIQUID && g_elem_density_q[t] < density);
}

// A free-surface cell of piece `node` on row `top`, or false.
static bool find_source(const World *w, int32_t node, int top, ElementId e, int *sx) {
    int ch = node >> NODE_SHIFT;
    uint8_t id = (uint8_t)(node & ((1 << NODE_SHIFT) - 1));
    int bx = (ch % w->chunks_x) << CHUNK_SHIFT;
    if (top < 0 || top >= w->height || (top >> CHUNK_SHIFT) != ch / w->chunks_x) {
        return false; // the row is in another chunk than this piece
    }
    if (!w->chunks[ch].active) {
        return false; // frozen (outside the simulated area): no water is taken from there
    }
    for (int lx = 0; lx < CHUNK_SIZE; lx++) {
        size_t i = world_index(w, bx + lx, top);
        Cell c = w->cells[i];
        if (w->piece[i] == id && c.element == e && cell_is_still_liquid(c) &&
            liquid_surface_free(w, bx + lx, top, e)) {
            *sx = bx + lx;
            return true;
        }
    }
    return false;
}

// Liquid entering gas compresses it, liquid leaving lets it expand. The change is spread over the
// gas around (world_gas_region), so no single cell jumps in pressure: one cell of gas volume moved
// at one spot would change the pressure there by tens of percent, enough to push liquid around
// (1 atm is 207 cells of water) and start a cascade.
// Gas at the opening and above the source within this relative pressure: one region; the gas
// simply trades places with the moving liquid (no compression).
#define GAS_SAME_REGION 0.05f
// A body may compress or expand gas (transfers between different gas regions, or between gas and
// vacuum) only once every this many ticks. Gas pressure evens out by diffusion, far slower than
// in reality; faster transfers would thin out or squeeze the gas next to the liquid faster than
// the rest of the room catches up, and the liquid would then chase those local differences.
#define GAS_LIQUID_INTERVAL 2

// Jets (M4b): from this outflow speed (cells per tick; about 9 cells of depth) water leaves an
// opening as free particles, at most JET_MAX_PER_REQUEST per opening cell per tick, with a spread
// across the jet of +-JET_SPREAD/2 of its speed.
#define JET_MIN_SPEED       1.0f
#define JET_MAX_PER_REQUEST 8
#define JET_SPREAD          0.06f

// Liquid arrives at (qx, qy), which holds gas: squeeze that gas into the gas around it (not
// (sx, sy)). Returns false if there is none (then it trades places).
static bool squeeze_gas(World *w, int qx, int qy, int sx, int sy) {
    size_t iq = world_index(w, qx, qy);
    int32_t found[GAS_REGION_CELLS];
    int n = world_gas_region(w, qx, qy, sx, sy, GAS_REGION_RADIUS, GAS_REGION_CELLS, found);
    if (n == 0) {
        return false;
    }
    world_gas_spread_add(w, found, n, &w->gas[iq], w->temp[iq]);
    for (int k = 0; k < n; k++) {
        world_note_moved(w, found[k] % w->width, found[k] / w->width);
    }
    world_wake_rect(w, qx - GAS_REGION_RADIUS, qy - GAS_REGION_RADIUS, qx + GAS_REGION_RADIUS,
                    qy + GAS_REGION_RADIUS);
    return true;
}

// Liquid left (sx, sy), now void. If gas lies above it, gas flows in from the gas around it,
// leaving no vacuum hole at the surface to pull liquid back in.
static void refill_with_gas(World *w, int sx, int sy) {
    if (sy <= 0) {
        return;
    }
    size_t ia = world_index(w, sx, sy - 1);
    ElementId g = w->cells[ia].element;
    if (g_elem_behavior[g] != BEHAVIOR_GAS) {
        return;
    }
    int32_t found[GAS_REGION_CELLS];
    int n = world_gas_region(w, sx, sy, sx, sy, GAS_REGION_RADIUS, GAS_REGION_CELLS, found);
    if (n == 0) {
        return;
    }
    float t;
    GasMix m;
    world_gas_spread_take(w, found, n, &m, &t);
    size_t is = world_index(w, sx, sy);
    Cell c = w->cells[ia];
    c.stamp = (uint8_t)w->tick;
    c.aux = 0;
    w->cells[is] = c;
    w->gas[is] = m;
    w->temp[is] = t;
    world_rho_note(w, sx, sy); // a new gas cell: its cached density is stale (sim.c, buoyancy)
    world_note_moved(w, sx, sy);
    world_wake_rect(w, sx - GAS_REGION_RADIUS, sy - GAS_REGION_RADIUS, sx + GAS_REGION_RADIUS,
                    sy + GAS_REGION_RADIUS);
}

// Takes the next free-surface cell of body `root` as a source: from its top; once those are used
// up, from the rows just below it, as long as their head stays at least one cell above q_head.
// Returns false if there is none.
static bool take_source(World *w, int32_t root, ElementId e, int32_t q_head, int *sx, int *sy) {
    PressureState *ps = w->pressure;
    int32_t top = ps->top[root];
    int k_max = (int)PR_MIN((int64_t)SOURCE_ROWS, (int64_t)q_head - 1 - top);
    int k = ps->used[root];
    for (; k <= k_max; k++) {
        int tries = 0;
        for (int32_t node = ps->src_head[root]; node >= 0 && tries < MAX_SOURCES;
             node = ps->src_next[node], tries++) {
            const Piece *pp = &ps->cp[node >> NODE_SHIFT].piece[node & ((1 << NODE_SHIFT) - 1)];
            *sy = pp->top_row + k;
            if (find_source(w, node, *sy, e, sx)) {
                ps->used[root] = (int16_t)k; // rows above are used up: later requests start here
                return true;
            }
        }
    }
    if (k > SOURCE_ROWS) {
        ps->top[root] = PRESSURE_NO_TOP; // used up for this tick: stop until relabelled
    }
    return false;
}

// Liquid leaves the source (sx, sy): the cell becomes void, and gas above it (if any) flows in.
static void vacate_source(World *w, int sx, int sy) {
    size_t is = world_index(w, sx, sy);
    w->cells[is] = (Cell){ELEM_VOID, (uint8_t)w->tick, w->cells[is].shade, 0};
    w->temp[is] = 0.0f;
    gas_mix_clear(&w->gas[is]);
    w->piece[is] = 0;
    refill_with_gas(w, sx, sy);
    world_wake_rect(w, sx - 1, sy - 1, sx + 1, sy + 1);
    world_note_moved(w, sx, sy);
}

// Torricelli: liquid at depth d (cells of head above the opening) leaves at sqrt(2 g d). From
// JET_MIN_SPEED it leaves as a jet of free particles: about `speed` cells per tick through the
// opening (the volume a jet of that speed carries), at most JET_MAX_PER_REQUEST.
static float outflow_speed(int32_t depth) {
    float v = sqrtf(2.0f * GRAVITY * CELL_SIZE_M * (float)depth); // m/s
    return v / CELL_SIZE_M * TICK_SECONDS;                        // cells per tick
}

// Launches up to n cells of the body as a jet from Q in direction (dx, dy) at `speed`; the k-th of
// n starts k/n of a tick further along, so they leave as a stream. The first source is given.
// Returns how many left.
static int launch_jet(World *w, int32_t root, ElementId e, int32_t q_head, int qx, int qy, int dx,
                      int dy, float speed, int n, int sx, int sy) {
    int launched = 0;
    for (int j = 0; j < n; j++) {
        if (j > 0 && !take_source(w, root, e, q_head, &sx, &sy)) {
            break;
        }
        if (w->particle_count >= PARTICLE_MAX) {
            break;
        }
        size_t is = world_index(w, sx, sy);
        Cell flying = w->cells[is];
        flying.aux = 0;
        // A slight spread across the jet, from a hash (deterministic, order-independent).
        uint64_t h = pr_splitmix64(w->seed ^ (w->tick << 20) ^ ((uint64_t)is << 3) ^ (uint64_t)j);
        float jitter = ((float)(h >> 40) * (1.0f / 16777216.0f) - 0.5f) * JET_SPREAD * speed;
        Particle p = {(float)qx + 0.5f,
                      (float)qy + 0.5f,
                      speed * (float)dx - jitter * (float)dy,
                      speed * (float)dy + jitter * (float)dx,
                      flying,
                      w->temp[is]};
        vacate_source(w, sx, sy);
        particle_add(w, &p);
        if (particle_step(w, &w->particles[w->particle_count - 1], (float)j / (float)n)) {
            w->particle_count--; // landed already (a wall right in front)
        }
        launched++;
    }
    world_wake_rect(w, qx - 1, qy - 1, qx + 1, qy + 1); // keeps the cells at the opening going
    w->launched_by_pressure += (uint32_t)launched;
    return launched;
}

int pressure_apply(World *w, const PressureRequest *req, int count) {
    PressureState *ps = w->pressure;
    int moved = 0;
    for (int r = 0; r < count; r++) {
        int px = req[r].px, py = req[r].py, qx = req[r].qx, qy = req[r].qy;
        size_t ip = world_index(w, px, py);
        Cell pc = w->cells[ip];
        uint8_t pid = w->piece[ip];
        if (!pid || !cell_is_still_liquid(pc) ||
            !open_for(w, qx, qy, g_elem_density_q[pc.element])) {
            continue;
        }
        int ch = (py >> CHUNK_SHIFT) * w->chunks_x + (px >> CHUNK_SHIFT);
        int32_t root = ps->root[ch << NODE_SHIFT | pid];
        int32_t top = ps->top[root];
        int32_t q_head = qy - pressure_load_head(w, qx, qy, pc.element, PRESSURE_LOAD_SCAN);
        int slack = w->cells[world_index(w, qx, qy)].element == ELEM_GAS ? PRESSURE_GAS_SLACK : 0;
        if (top == PRESSURE_NO_TOP || top + ps->used[root] > q_head - 1 - slack) {
            continue;
        }
        int used0 = ps->used[root];
        int sx = -1, sy = 0;
        if (!take_source(w, root, pc.element, q_head - slack, &sx, &sy)) {
            world_wake_rect(w, px, py, px, py);
            continue;
        }
        size_t is = world_index(w, sx, sy), iq = world_index(w, qx, qy);
        // Gas at Q: if it is at about the pressure of the gas above the source, it is the same
        // region, and it simply takes the source's place. Otherwise it is compressed into the
        // gas around Q, and the gas above the source expands into it. Such transfers are rate
        // limited per body (GAS_LIQUID_INTERVAL).
        bool q_gas = g_elem_behavior[w->cells[iq].element] == BEHAVIOR_GAS;
        float pq = world_gas_pressure(w, qx, qy), ps_above = world_gas_pressure(w, sx, sy - 1);
        bool same_region = q_gas && ps_above > 0.0f &&
                           fabsf(pq - ps_above) <= GAS_SAME_REGION * PR_MAX(pq, ps_above);
        if ((q_gas || ps_above > 0.0f) && !same_region) {
            if (w->tick < ps->gas_tick[root] + GAS_LIQUID_INTERVAL) {
                ps->used[root] = (int16_t)used0;    // the source was not taken
                world_wake_rect(w, px, py, px, py); // retry later
                continue;
            }
            ps->gas_tick[root] = w->tick;
        }
        // A hole in a side wall, below the surface (P is covered: liquid or a wall above it),
        // deep enough: the liquid leaves as a jet of free particles. Openings at a free surface
        // (above P, or beside a surface cell) only level the liquid, cell by cell: a surface
        // does not spout.
        float speed = outflow_speed(q_head - (top + used0));
        // The jet also needs somewhere to go: the cell beyond Q must be open too, or hold falling
        // liquid (moving away), not be part of a bubble of void inside the liquid (which is
        // simply filled).
        bool submerged = !liquid_surface_free(w, px, py, pc.element);
        int bx = qx + (qx - px);
        Cell beyond = w->cells[world_index(w, PR_CLAMP(bx, 0, w->width - 1), qy)];
        bool room = particle_passes(w, bx, qy) ||
                    (world_in_bounds(w, bx, qy) &&
                     g_elem_behavior[beyond.element] == BEHAVIOR_LIQUID && (beyond.aux & 0x7F));
        if (qy == py && submerged && room && speed >= JET_MIN_SPEED &&
            w->particle_count < PARTICLE_MAX) {
            uint64_t h = pr_splitmix64(w->seed ^ (w->tick * 0x9E3779B97F4A7C15ull) ^
                                       (uint64_t)world_index(w, qx, qy));
            float frac = speed - floorf(speed);
            int n = (int)speed + ((float)(h >> 40) * (1.0f / 16777216.0f) < frac ? 1 : 0);
            n = PR_CLAMP(n, 1, JET_MAX_PER_REQUEST);
            moved +=
                launch_jet(w, root, pc.element, q_head, qx, qy, qx - px, qy - py, speed, n, sx, sy);
            continue;
        }
        Cell moving = w->cells[is];
        Cell replaced = w->cells[iq];
        moving.aux &= 0x80; // arrives at rest
        moving.stamp = replaced.stamp = (uint8_t)w->tick;
        float t = w->temp[is];
        if (q_gas && !same_region) {
            // Gas at Q that cannot be squeezed (a bubble alone under the liquid) must not trade
            // places: it would land at the source, which may be far away, in other gas or in
            // vacuum (a bubble from a pool teleported into a sealed barometer tube). No transfer;
            // the bubble rises on its own.
            if (!squeeze_gas(w, qx, qy, sx, sy)) {
                ps->used[root] = (int16_t)used0;
                continue;
            }
            replaced = (Cell){ELEM_VOID, replaced.stamp, replaced.shade, 0};
            w->temp[iq] = 0.0f;
            gas_mix_clear(&w->gas[iq]);
        } else if (g_elem_behavior[replaced.element] == BEHAVIOR_LIQUID) {
            // A lighter liquid at Q: liquid entering under it lifts the column above Q by one cell.
            // Its cell goes to the first free cell above that column (whose void or gas takes the
            // source's place), never to the source, which may be far away (another arm of a
            // U-tube). Without a free cell above, no transfer.
            int ty = qy - 1;
            while (ty > qy - PRESSURE_LOAD_SCAN && ty >= 0 &&
                   g_elem_behavior[world_elem(w, qx, ty)] == BEHAVIOR_LIQUID) {
                ty--;
            }
            if (ty < 0 || !particle_passes(w, qx, ty)) {
                ps->used[root] = (int16_t)used0;
                continue;
            }
            size_t it = world_index(w, qx, ty);
            Cell above = w->cells[it];
            float tt = w->temp[it];
            GasMix mt = w->gas[it];
            w->cells[it] = replaced;
            w->temp[it] = w->temp[iq];
            gas_mix_clear(&w->gas[it]);
            above.stamp = replaced.stamp;
            replaced = above;
            w->temp[iq] = tt;
            w->gas[iq] = mt;
            world_wake_rect(w, qx - 1, ty - 1, qx + 1, ty + 1);
            world_note_moved(w, qx, ty);
            world_rho_note(w, qx, ty);
        }
        w->cells[is] = replaced;
        w->temp[is] = w->temp[iq];
        w->gas[is] = w->gas[iq];
        w->cells[iq] = moving;
        w->temp[iq] = t;
        gas_mix_clear(&w->gas[iq]);
        if (w->cells[is].element == ELEM_VOID) {
            refill_with_gas(w, sx, sy);
        }
        w->piece[is] = 0; // no longer a source this tick
        w->piece[iq] = 0;
        // Gas may have been written here, and its cached density is stale (sim.c, buoyancy).
        world_rho_note(w, sx, sy);
        world_rho_note(w, qx, qy);
        world_wake_rect(w, sx - 1, sy - 1, sx + 1, sy + 1);
        world_wake_rect(w, qx - 1, qy - 1, qx + 1, qy + 1);
        world_note_moved(w, qx, qy);
        world_note_moved(w, sx, sy);
        moved++;
    }
    return moved;
}
