#include "sim/edit.h"

#include "core/rng.h"

bool edit_queue_push(EditQueue *q, const Edit *e) {
    if (q->count >= EDIT_QUEUE_CAP) {
        return false;
    }
    q->items[q->count++] = *e;
    return true;
}

void sim_apply_edits(World *w, EditQueue *q) {
    for (int i = 0; i < q->count; i++) {
        sim_apply_edit(w, &q->items[i]);
    }
    q->count = 0;
}

// Distance test: is cell (px, py) within `radius` (+0.5, so radius 0 is one pixel) of the
// segment a-b, measured from the cell centre?
static bool in_capsule(float px, float py, float ax, float ay, float bx, float by, float r) {
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (len2 > 0.0f) {
        t = ((px - ax) * dx + (py - ay) * dy) / len2;
        t = PR_CLAMP(t, 0.0f, 1.0f);
    }
    float qx = ax + t * dx - px, qy = ay + t * dy - py;
    float rr = r + 0.5f;
    return qx * qx + qy * qy <= rr * rr;
}

// Liang-Barsky: does segment a-b touch the axis-aligned box centred on (px, py) with half-size
// h? That is the square brush swept along the segment.
static bool clip(float p, float q, float *t0, float *t1) {
    if (p == 0.0f) {
        return q >= 0.0f;
    }
    float r = q / p;
    if (p < 0.0f) {
        if (r > *t1) {
            return false;
        }
        if (r > *t0) {
            *t0 = r;
        }
    } else {
        if (r < *t0) {
            return false;
        }
        if (r < *t1) {
            *t1 = r;
        }
    }
    return true;
}

static bool in_swept_square(float px, float py, float ax, float ay, float bx, float by, float h) {
    float t0 = 0.0f, t1 = 1.0f;
    float dx = bx - ax, dy = by - ay;
    return clip(-dx, ax - (px - h), &t0, &t1) && clip(dx, (px + h) - ax, &t0, &t1) &&
           clip(-dy, ay - (py - h), &t0, &t1) && clip(dy, (py + h) - ay, &t0, &t1);
}

// The Air brush paints oxygen with this probability where no neighbour is oxygen (tuned so that
// AIR_O2_FRAC of the cells are oxygen; tested).
#define AIR_O2_PAINT_P 0.335f

// Small clumps (docs/SIMULATION.md -> Powders and digging): a group of at most CLUMP_MAX_CELLS
// solid cells (4-connected) that holds on to nothing else breaks loose, so the player never gets
// stuck on a few pixels left floating after digging or shooting.
#define CLUMP_MAX_CELLS 20

ElementId elem_loosened(ElementId e) {
    return e == ELEM_DIRT ? ELEM_SOIL : e == ELEM_ROCK ? ELEM_RUBBLE : e;
}

// The clump of solid cells 4-connected to cell i (dirt or rock), into cells[]. Returns its size,
// or 0 if it is bigger than CLUMP_MAX_CELLS, touches the world's edge, or holds on to a solid
// that does not break loose (iron, ice). Creatures' bodies hold nothing up.
static int clump_collect(const World *w, int32_t i, int32_t *cells) {
    int n = 0, head = 0;
    cells[n++] = i;
    while (head < n) {
        int32_t c = cells[head++];
        int cx = c % w->width, cy = c / w->width;
        static const int8_t k_d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int k = 0; k < 4; k++) {
            int nx = cx + k_d[k][0], ny = cy + k_d[k][1];
            if (!world_in_bounds(w, nx, ny)) {
                return 0;
            }
            int32_t ni = (int32_t)world_index(w, nx, ny);
            ElementId e = w->cells[ni].element;
            if (g_elem_behavior[e] != BEHAVIOR_STATIC || e == ELEM_BODY) {
                continue;
            }
            if (elem_loosened(e) == e) {
                return 0;
            }
            bool seen = false;
            for (int j = 0; j < n && !seen; j++) {
                seen = cells[j] == ni;
            }
            if (seen) {
                continue;
            }
            if (n == CLUMP_MAX_CELLS) {
                return 0;
            }
            cells[n++] = ni;
        }
    }
    return n;
}

int64_t sim_loosen_small_clumps(World *w, int x0, int y0, int x1, int y1) {
    x0 = PR_MAX(x0, 0);
    y0 = PR_MAX(y0, 0);
    x1 = PR_MIN(x1, w->width - 1);
    y1 = PR_MIN(y1, w->height - 1);
    int64_t loosened = 0;
    int32_t cells[CLUMP_MAX_CELLS];
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int32_t i = (int32_t)world_index(w, x, y);
            ElementId e = w->cells[i].element;
            if (elem_loosened(e) == e) {
                continue;
            }
            int n = clump_collect(w, i, cells);
            for (int k = 0; k < n; k++) {
                Cell *c = &w->cells[cells[k]];
                c->element = elem_loosened(c->element);
                c->aux = 0;
                c->stamp = (uint8_t)(w->tick - 1);
                int cx = cells[k] % w->width, cy = cells[k] / w->width;
                world_wake_rect(w, cx - 1, cy - 1, cx + 1, cy + 1);
                world_note_moved(w, cx, cy);
            }
            loosened += n;
        }
    }
    return loosened;
}

static int64_t apply_segment(World *w, const Edit *e, int ax, int ay, int bx, int by, Rng *rng) {
    int r = e->radius;
    int x0 = PR_MAX(PR_MIN(ax, bx) - r, 0);
    int y0 = PR_MAX(PR_MIN(ay, by) - r, 0);
    int x1 = PR_MIN(PR_MAX(ax, bx) + r, w->width - 1);
    int y1 = PR_MIN(PR_MAX(ay, by) + r, w->height - 1);
    if (x0 > x1 || y0 > y1) {
        return 0;
    }
    int64_t written = 0;
    float fr = (float)r;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            bool inside = e->shape == EDIT_SQUARE
                              ? in_swept_square((float)x, (float)y, (float)ax, (float)ay, (float)bx,
                                                (float)by, fr + 0.5f)
                              : in_capsule((float)x, (float)y, (float)ax, (float)ay, (float)bx,
                                           (float)by, fr);
            if (!inside) {
                continue;
            }
            size_t i = world_index(w, x, y);
            if (w->cells[i].element == ELEM_BODY && e->mode == EDIT_PAINT) {
                continue; // creatures' bodies are moved only by their creature
            }
            if (e->mode == EDIT_DIG) {
                // Digging loosens the solid: same temperature, now a powder that falls.
                ElementId el = w->cells[i].element;
                ElementId to = elem_loosened(el);
                if (to != el) {
                    w->cells[i].element = to;
                    w->cells[i].aux = 0;
                    w->cells[i].stamp = (uint8_t)(w->tick - 1);
                    written++;
                }
                continue;
            }
            if (e->mode != EDIT_PAINT) {
                // Heat or cool. The stored temperature may go beyond a phase-change point (that
                // is latent heat); the physical temperature is what is limited.
                ElementId el = w->cells[i].element;
                if (g_elem_cap[el] == 0.0f) {
                    continue;
                }
                float t =
                    w->temp[i] + (e->mode == EDIT_HEAT ? EDIT_HEAT_STEP_K : -EDIT_HEAT_STEP_K);
                float tp = elem_temp(el, t);
                if (tp < 1.0f || tp > EDIT_HEAT_MAX_K) {
                    continue;
                }
                w->temp[i] = t;
                written++;
                continue;
            }
            if (e->only_void && w->cells[i].element != ELEM_VOID) {
                continue;
            }
            uint32_t bits = rng_next(rng);
            ElementId el = e->element;
            if (el == EDIT_AIR) {
                // Air is mixed molecule by molecule: no two oxygen cells side by side (the most
                // even mix one gas per cell allows, which diffusion leaves alone, so painted air
                // is settled at once). Oxygen with probability AIR_O2_PAINT_P where no neighbour
                // is oxygen: about AIR_O2_FRAC of the cells.
                bool o2_near = false;
                static const int8_t k_n[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (int k = 0; k < 4; k++) {
                    ElementId n = world_elem(w, x + k_n[k][0], y + k_n[k][1]);
                    o2_near |= n == ELEM_OXYGEN;
                }
                el = !o2_near && (bits >> 16) < (uint32_t)(AIR_O2_PAINT_P * 65536.0f)
                         ? ELEM_OXYGEN
                         : ELEM_NITROGEN;
            }
            world_set_cell(w, x, y, el, bits);
            written++;
        }
    }
    world_wake_rect(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
    world_wake_heat(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
    if (e->mode == EDIT_DIG && written > 0) {
        sim_loosen_small_clumps(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
    }
    return written;
}

int64_t sim_apply_edit(World *w, const Edit *e) {
    if ((e->element >= ELEM_COUNT && e->element != EDIT_AIR) || e->radius < 0) {
        return 0;
    }
    Rng rng = rng_from_seed(w->seed ^ pr_splitmix64(w->tick + 0x5EED) ^
                            ((uint64_t)(uint32_t)e->x1 << 32) ^ (uint32_t)e->y1);

    // Split long strokes into short pieces so each piece's bounding box stays small.
    int dx = e->x1 - e->x0, dy = e->y1 - e->y0;
    int len = PR_MAX(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
    int piece = PR_MAX(4, e->radius);
    int pieces = PR_MAX(1, (len + piece - 1) / piece);
    int64_t written = 0;
    int ax = e->x0, ay = e->y0;
    for (int i = 1; i <= pieces; i++) {
        int bx = e->x0 + (int)((int64_t)dx * i / pieces);
        int by = e->y0 + (int)((int64_t)dy * i / pieces);
        written += apply_segment(w, e, ax, ay, bx, by, &rng);
        ax = bx;
        ay = by;
    }
    return written;
}
