#include "render/render.h"

#include "core/log.h"

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
    if (!r->pixels || !r->dirty_list) {
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

static void convert_chunk(void *user, int index) {
    ConvertJob *job = user;
    WorldRenderer *r = job->r;
    const World *w = job->w;
    int chunk = r->dirty_list[index];
    int x0 = (chunk % w->chunks_x) << CHUNK_SHIFT;
    int y0 = (chunk / w->chunks_x) << CHUNK_SHIFT;
    for (int y = y0; y < y0 + CHUNK_SIZE; y++) {
        const Cell *src = &w->cells[world_index(w, x0, y)];
        uint32_t *dst = &r->pixels[(size_t)y * r->width + x0];
        for (int i = 0; i < CHUNK_SIZE; i++) {
            dst[i] = r->lut[src[i].element][src[i].shade];
        }
    }
}

void render_upload(WorldRenderer *r, World *w, int cx0, int cy0, int cx1, int cy1) {
    cx0 = PR_MAX(cx0, 0);
    cy0 = PR_MAX(cy0, 0);
    cx1 = PR_MIN(cx1, w->chunks_x - 1);
    cy1 = PR_MIN(cy1, w->chunks_y - 1);
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
