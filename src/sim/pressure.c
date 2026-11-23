#include "sim/pressure.h"

#include "core/jobs.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PIECES  255 // body pieces per chunk; further pieces get no pressure until relabelled
#define NODE_SHIFT  8   // node = chunk index << NODE_SHIFT | piece id
#define MAX_SOURCES 16  // pieces tried per transfer when looking for a top cell
#define SOURCE_ROWS 8   // rows below the body's top also searched once the top row is used up

typedef struct Piece {
    int16_t top_y; // highest free-surface row of this piece, PRESSURE_NO_TOP if none
    uint8_t element;
} Piece;

typedef struct ChunkPieces {
    uint16_t count;
    int16_t last_top;            // best body top among this chunk's pieces at the previous rebuild
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
    int16_t *top;      // the body's highest free-surface row
    int32_t *src_head; // first node of the list above
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
    ps->top = pr_aligned_alloc_zero(64, sizeof(int16_t) * nodes);
    ps->src_head = pr_aligned_alloc_zero(64, sizeof(int32_t) * nodes);
    if (!ps->cp || !ps->jobs || !ps->parent || !ps->root || !ps->src_next || !ps->top ||
        !ps->src_head) {
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
            int16_t top = PRESSURE_NO_TOP;
            int sp = 0;
            stack[sp++] = (uint16_t)(ly << CHUNK_SHIFT | lx);
            visited[ly][lx] = 1;
            while (sp > 0) {
                int v = stack[--sp];
                int vx = v & CHUNK_MASK, vy = v >> CHUNK_SHIFT;
                int x = bx + vx, y = by + vy;
                w->piece[world_index(w, x, y)] = (uint8_t)id;
                if (y < top && liquid_surface_free(w, x, y, e)) {
                    top = (int16_t)y;
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
                cp->piece[id] = (Piece){top, e};
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
            ps->src_head[r] = -1;
        }
    }
    for (int ch = 0; ch < ps->chunks; ch++) {
        for (int id = 1; id <= ps->cp[ch].count; id++) {
            int32_t r = ps->root[ch << NODE_SHIFT | id];
            int16_t t = ps->cp[ch].piece[id].top_y;
            if (t < ps->top[r]) {
                ps->top[r] = t;
            }
        }
    }
    for (int ch = ps->chunks - 1; ch >= 0; ch--) {
        for (int id = ps->cp[ch].count; id >= 1; id--) {
            int32_t node = ch << NODE_SHIFT | id;
            int32_t r = ps->root[node];
            if (ps->cp[ch].piece[id].top_y == ps->top[r] && ps->top[r] != PRESSURE_NO_TOP) {
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
        int16_t best = PRESSURE_NO_TOP;
        for (int id = 1; id <= ps->cp[ch].count; id++) {
            int16_t t = ps->top[ps->root[ch << NODE_SHIFT | id]];
            if (t < best) {
                best = t;
            }
        }
        if (best < ps->cp[ch].last_top) {
            Chunk *c = &w->chunks[ch];
            dirty_rect_add(&c->cur, 0, 0, CHUNK_SIZE - 1, CHUNK_SIZE - 1);
        }
        ps->cp[ch].last_top = best;
    }
}

int pressure_body_top(const World *w, int x, int y) {
    uint8_t id = w->piece[world_index(w, x, y)];
    if (!id) {
        return PRESSURE_NO_TOP;
    }
    int ch = (y >> CHUNK_SHIFT) * w->chunks_x + (x >> CHUNK_SHIFT);
    const PressureState *ps = w->pressure;
    return ps->top[ps->root[ch << NODE_SHIFT | id]];
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
        int top = ps->top[root];
        if (top == PRESSURE_NO_TOP || top > qy - 1) {
            continue;
        }
        // Take a free-surface cell from the body's top row; once that row is used up, from the
        // rows just below it, as long as they are still at least one row above Q.
        int sx = -1, sy = top;
        for (; sy <= PR_MIN(qy - 1, top + SOURCE_ROWS) && sx < 0; sy++) {
            int tries = 0;
            for (int32_t node = ps->src_head[root]; node >= 0 && tries < MAX_SOURCES;
                 node = ps->src_next[node], tries++) {
                if (find_source(w, node, sy, pc.element, &sx)) {
                    break;
                }
            }
            if (sx >= 0) {
                break;
            }
        }
        if (sx < 0) {
            if (sy > top + SOURCE_ROWS) {
                ps->top[root] = PRESSURE_NO_TOP; // used up for this tick: stop until relabelled
            }
            world_wake_rect(w, px, py, px, py);
            continue;
        }
        ps->top[root] = (int16_t)sy; // rows above are used up: later requests start here
        size_t is = world_index(w, sx, sy), iq = world_index(w, qx, qy);
        Cell moving = w->cells[is];
        Cell replaced = w->cells[iq];
        moving.aux &= 0x80; // arrives at rest
        moving.stamp = replaced.stamp = (uint8_t)w->tick;
        w->cells[iq] = moving;
        w->cells[is] = replaced;
        float t = w->temp[is];
        w->temp[is] = w->temp[iq];
        w->temp[iq] = t;
        w->piece[is] = 0; // no longer a source this tick
        w->piece[iq] = 0;
        world_wake_rect(w, sx - 1, sy - 1, sx + 1, sy + 1);
        world_wake_rect(w, qx - 1, qy - 1, qx + 1, qy + 1);
        world_note_moved(w, qx, qy);
        world_note_moved(w, sx, sy);
        moved++;
    }
    return moved;
}
