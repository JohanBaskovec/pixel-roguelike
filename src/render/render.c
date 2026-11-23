#include "render/render.h"

#include "core/log.h"
#include "sim/particle.h"
#include "sim/sim.h"

#include <math.h>

static uint32_t channel(uint32_t c, int shift) {
    return (c >> shift) & 0xFF;
}

static int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// Base colour, brightness varied by shade, then blended over the void colour by alpha.
static void build_lut(WorldRenderer *r) {
    uint32_t bg = g_elem_defs[ELEM_VOID].color;
    for (int e = 0; e < ELEM_COUNT; e++) {
        const ElementDef *d = &g_elem_defs[e];
        for (int s = 0; s < 256; s++) {
            int var = ((s - 128) * d->shade_var) / 128;
            int a = d->alpha;
            uint32_t out = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                int fg = clamp255((int)channel(d->color, shift) + var);
                int b = (int)channel(bg, shift);
                int v = (fg * a + b * (255 - a)) / 255;
                out |= (uint32_t)clamp255(v) << shift;
            }
            r->lut[e][s] = 0xFF000000u | out;
            uint32_t solid = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                solid |= (uint32_t)clamp255((int)channel(d->color, shift) + var) << shift;
            }
            r->lut_solid[e][s] = solid;
        }
    }
}

bool render_init(WorldRenderer *r, SDL_Renderer *sdl, World *w) {
    *r = (WorldRenderer){0};
    r->sdl = sdl;
    r->width = w->width;
    r->height = w->height;
    r->texture = SDL_CreateTexture(sdl, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                   w->width, w->height);
    if (!r->texture) {
        pr_log_error("SDL_CreateTexture (%dx%d) failed: %s", w->width, w->height, SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(r->texture, SDL_SCALEMODE_NEAREST);
    r->pixels = pr_aligned_alloc_zero(64, (size_t)w->width * w->height * sizeof(uint32_t));
    r->dirty_list = pr_aligned_alloc_zero(64, (size_t)w->chunks_x * w->chunks_y * sizeof(int));
    r->had_particles = pr_aligned_alloc_zero(64, (size_t)w->chunks_x * w->chunks_y);
    if (!r->pixels || !r->dirty_list || !r->had_particles) {
        pr_log_error("render_init: out of memory");
        return false;
    }
    build_lut(r);
    render_invalidate(w);
    return true;
}

void render_shutdown(WorldRenderer *r) {
    if (r->texture) {
        SDL_DestroyTexture(r->texture);
    }
    pr_aligned_free(r->pixels);
    pr_aligned_free(r->dirty_list);
    pr_aligned_free(r->had_particles);
    *r = (WorldRenderer){0};
}

void render_invalidate(World *w) {
    for (int i = 0; i < w->chunks_x * w->chunks_y; i++) {
        w->chunks[i].render_dirty = 1;
    }
}

typedef struct ConvertJob {
    WorldRenderer *r;
    const World *w;
} ConvertJob;

static uint32_t mix(uint32_t a, uint32_t b, int t, int span) {
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        int ca = (int)((a >> shift) & 0xFF), cb = (int)((b >> shift) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * t / span) << shift;
    }
    return out;
}

// A gas cell's colour mixes its species' colours, each weighted by its opacity at 1 atm times
// its mole fraction; the cell's opacity is that weighted sum, times sqrt(pressure / 1 atm) between
// GAS_ALPHA_MIN (thin gas stays faintly visible) and GAS_ALPHA_MAX.
#define GAS_ALPHA_MIN 0.3f
#define GAS_ALPHA_MAX 2.5f

static uint32_t gas_color(const WorldRenderer *r, const World *w, size_t i, Cell c) {
    const GasMix *g = &w->gas[i];
    float n = gas_moles(g);
    if (n <= 0.0f) {
        return g_elem_defs[ELEM_VOID].color | 0xFF000000u;
    }
    float acc[3] = {0.0f, 0.0f, 0.0f}, wsum = 0.0f;
    for (int s = 0; s < GAS_SPECIES; s++) {
        float x = g->m[s] * g_species_inv_molar[s] / n;
        if (x <= 0.0f) {
            continue;
        }
        ElementId e = g_species_elem[s];
        float wt = (float)g_elem_defs[e].alpha * x;
        uint32_t col = r->lut_solid[e][c.shade];
        for (int k = 0; k < 3; k++) {
            acc[k] += wt * (float)channel(col, 8 * k);
        }
        wsum += wt;
    }
    float f =
        PR_CLAMP(sqrtf(gas_mix_pressure(g, w->temp[i]) / P_ATM), GAS_ALPHA_MIN, GAS_ALPHA_MAX);
    int a = (int)PR_MIN(wsum * f, 235.0f);
    uint32_t bg = g_elem_defs[ELEM_VOID].color;
    uint32_t out = 0xFF000000u;
    for (int k = 0; k < 3; k++) {
        int fg = wsum > 0.0f ? (int)(acc[k] / wsum) : 0;
        int v = (fg * a + (int)channel(bg, 8 * k) * (255 - a)) / 255;
        out |= (uint32_t)v << (8 * k);
    }
    return out;
}

// Pressure view, gases: log scale from 0.01 atm (dark blue) through 1 atm (green), 10 atm
// (yellow), 100 atm (red) to 1000 atm and above (white).
static uint32_t gas_pressure_color(float p) {
    static const uint32_t stops[6] = {0x102040, 0x2858B0, 0x30A060, 0xE0C030, 0xE04020, 0xFFFFFF};
    float l = log10f(PR_MAX(p, 1.0f) / P_ATM) + 2.0f; // 0 at 0.01 atm, 5 at 1000 atm
    l = PR_CLAMP(l, 0.0f, 4.999f);
    int k = (int)l;
    return mix(stops[k], stops[k + 1], (int)((l - (float)k) * 256.0f), 256);
}

// Pressure view: still liquid coloured by depth below its body's top (light cyan at the surface,
// blue, then purple and red deep down); gases by pressure; everything else dimmed.
static uint32_t pressure_color(const World *w, const WorldRenderer *r, int x, int y, Cell c) {
    if (g_elem_behavior[c.element] == BEHAVIOR_GAS) {
        return gas_pressure_color(world_gas_pressure(w, x, y));
    }
    int depth = sim_pressure_depth(w, x, y);
    if (depth >= 0) {
        if (depth < 64) {
            return mix(0x9FEFFF, 0x2850E0, depth, 64);
        }
        if (depth < 256) {
            return mix(0x2850E0, 0x9030C0, depth - 64, 192);
        }
        return mix(0x9030C0, 0xF03050, PR_MIN(depth - 256, 512), 512);
    }
    uint32_t base = r->lut[c.element][c.shade];
    return 0xFF000000u | ((base >> 2) & 0x3F3F3F);
}

// Temperature view: deep blue (-50 degC and below), cyan (0), dark grey-green (20), orange
// (100), red (500), yellow (1000), white (2000+). Void is black.
static uint32_t temperature_color(float t) {
    float c = t - 273.15f;
    if (c < -50.0f) {
        return 0xFF1830A0;
    }
    if (c < 0.0f) {
        return mix(0x1830A0, 0x40C8E0, (int)(c + 50.0f), 50);
    }
    if (c < 20.0f) {
        return mix(0x40C8E0, 0x3A4A40, (int)c, 20);
    }
    if (c < 100.0f) {
        return mix(0x3A4A40, 0xF08020, (int)(c - 20.0f), 80);
    }
    if (c < 500.0f) {
        return mix(0xF08020, 0xE02010, (int)(c - 100.0f), 400);
    }
    if (c < 1000.0f) {
        return mix(0xE02010, 0xFFE040, (int)(c - 500.0f), 500);
    }
    return mix(0xFFE040, 0xFFFFFF, (int)PR_MIN(c - 1000.0f, 1000.0f), 1000);
}

// Hot matter glows (roughly blackbody): from a dull red around 500 degC to white-hot.
static uint32_t glow(uint32_t base, float t) {
    float c = t - 273.15f;
    uint32_t hot;
    if (c < 900.0f) {
        hot = mix(0x801000, 0xFF5010, (int)(c - 480.0f), 420);
    } else if (c < 1500.0f) {
        hot = mix(0xFF5010, 0xFFB050, (int)(c - 900.0f), 600);
    } else {
        hot = mix(0xFFB050, 0xFFF4E0, (int)PR_MIN(c - 1500.0f, 1500.0f), 1500);
    }
    return mix(base, hot, (int)PR_MIN(c - 480.0f, 400.0f), 400);
}

#define GLOW_MIN_K (480.0f + 273.15f)

static void convert_chunk(void *user, int index) {
    ConvertJob *job = user;
    WorldRenderer *r = job->r;
    const World *w = job->w;
    int chunk = r->dirty_list[index];
    int x0 = (chunk % w->chunks_x) << CHUNK_SHIFT;
    int y0 = (chunk / w->chunks_x) << CHUNK_SHIFT;
    if (r->temperature_view) {
        for (int y = y0; y < y0 + CHUNK_SIZE; y++) {
            for (int i = 0; i < CHUNK_SIZE; i++) {
                size_t ci = world_index(w, x0 + i, y);
                Cell c = w->cells[ci];
                r->pixels[(size_t)y * r->width + x0 + i] =
                    g_elem_cap[c.element] > 0.0f
                        ? temperature_color(elem_temp(c.element, w->temp[ci]))
                        : 0xFF000000u;
            }
        }
        return;
    }
    if (r->pressure_view) {
        for (int y = y0; y < y0 + CHUNK_SIZE; y++) {
            for (int i = 0; i < CHUNK_SIZE; i++) {
                r->pixels[(size_t)y * r->width + x0 + i] =
                    pressure_color(w, r, x0 + i, y, w->cells[world_index(w, x0 + i, y)]);
            }
        }
        return;
    }
    for (int y = y0; y < y0 + CHUNK_SIZE; y++) {
        size_t row = world_index(w, x0, y);
        const Cell *src = &w->cells[row];
        const float *temp = &w->temp[row];
        uint32_t *dst = &r->pixels[(size_t)y * r->width + x0];
        for (int i = 0; i < CHUNK_SIZE; i++) {
            uint32_t color = g_elem_behavior[src[i].element] == BEHAVIOR_GAS
                                 ? gas_color(r, w, row + (size_t)i, src[i])
                                 : r->lut[src[i].element][src[i].shade];
            if (temp[i] > GLOW_MIN_K) {
                float tp = elem_temp(src[i].element, temp[i]);
                if (tp > GLOW_MIN_K && g_elem_cap[src[i].element] > 0.0f) {
                    color = glow(color, tp);
                }
            }
            dst[i] = color;
        }
    }
}

void render_upload(WorldRenderer *r, World *w, int cx0, int cy0, int cx1, int cy1) {
    cx0 = PR_MAX(cx0, 0);
    cy0 = PR_MAX(cy0, 0);
    cx1 = PR_MIN(cx1, w->chunks_x - 1);
    cy1 = PR_MIN(cy1, w->chunks_y - 1);
    // Free particles are drawn over the grid. Chunks they were drawn on last frame are redrawn
    // (erasing them), and so are the chunks they are on now (then drawn on).
    int n_chunks = w->chunks_x * w->chunks_y;
    for (int i = 0; i < n_chunks; i++) {
        if (r->had_particles[i]) {
            r->had_particles[i] = 0;
            w->chunks[i].render_dirty = 1;
        }
    }
    for (int i = 0; i < w->particle_count; i++) {
        int px = (int)w->particles[i].x, py = (int)w->particles[i].y;
        if (world_in_bounds(w, px, py)) {
            int ch = (py >> CHUNK_SHIFT) * w->chunks_x + (px >> CHUNK_SHIFT);
            r->had_particles[ch] = 1;
            w->chunks[ch].render_dirty = 1;
        }
    }
    int count = 0;
    for (int cy = cy0; cy <= cy1; cy++) {
        for (int cx = cx0; cx <= cx1; cx++) {
            Chunk *c = world_chunk(w, cx, cy);
            if (c->render_dirty) {
                c->render_dirty = 0;
                r->dirty_list[count++] = cy * w->chunks_x + cx;
            }
        }
    }
    r->chunks_uploaded = (uint32_t)count;
    if (count == 0) {
        return;
    }
    ConvertJob job = {r, w};
    jobs_run(w->jobs, count, convert_chunk, &job);
    if (!r->temperature_view && !r->pressure_view) {
        for (int i = 0; i < w->particle_count; i++) {
            const Particle *p = &w->particles[i];
            int px = (int)p->x, py = (int)p->y;
            if (px >> CHUNK_SHIFT >= cx0 && px >> CHUNK_SHIFT <= cx1 && py >> CHUNK_SHIFT >= cy0 &&
                py >> CHUNK_SHIFT <= cy1 && world_in_bounds(w, px, py)) {
                r->pixels[(size_t)py * r->width + px] = r->lut[p->cell.element][p->cell.shade];
            }
        }
    }

    // One upload per chunk row, spanning its dirty chunks (the list is in row order). Chunks in
    // between that were not dirty are re-sent unchanged; that is cheaper than many small uploads.
    int pitch = r->width * (int)sizeof(uint32_t);
    for (int i = 0; i < count;) {
        int row = r->dirty_list[i] / w->chunks_x;
        int first = r->dirty_list[i] % w->chunks_x, last = first;
        while (i < count && r->dirty_list[i] / w->chunks_x == row) {
            last = r->dirty_list[i] % w->chunks_x;
            i++;
        }
        SDL_Rect rect = {first << CHUNK_SHIFT, row << CHUNK_SHIFT,
                         (last - first + 1) << CHUNK_SHIFT, CHUNK_SIZE};
        SDL_UpdateTexture(r->texture, &rect, &r->pixels[(size_t)rect.y * r->width + rect.x], pitch);
    }
}

void render_draw(WorldRenderer *r, const SDL_FRect *src, const SDL_FRect *dst, bool smooth) {
    if (smooth != r->smooth) {
        SDL_SetTextureScaleMode(r->texture, smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        r->smooth = smooth;
    }
    SDL_RenderTexture(r->sdl, r->texture, src, dst);
}
