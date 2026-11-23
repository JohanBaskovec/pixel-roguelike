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
                el =
                    (bits >> 16) < (uint32_t)(AIR_O2_FRAC * 65536.0f) ? ELEM_OXYGEN : ELEM_NITROGEN;
            }
            world_set_cell(w, x, y, el, bits);
            written++;
        }
    }
    world_wake_rect(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
    world_wake_heat(w, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
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
