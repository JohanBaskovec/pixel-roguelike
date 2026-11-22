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
        pr_log_error("SDL_CreateTexture failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(r->texture, SDL_SCALEMODE_NEAREST);
    r->pixels = pr_aligned_alloc_zero(64, (size_t)w->width * w->height * sizeof(uint32_t));
    if (!r->pixels) {
        pr_log_error("render_init: out of memory");
        return false;
    }
    build_lut(r);
    for (int i = 0; i < w->chunks_x * w->chunks_y; i++) {
        w->chunks[i].render_dirty = 1;
    }
    return true;
}

void render_shutdown(WorldRenderer *r) {
    if (r->texture) {
        SDL_DestroyTexture(r->texture);
    }
    pr_aligned_free(r->pixels);
    *r = (WorldRenderer){0};
}

static void convert_chunk(WorldRenderer *r, const World *w, int cx, int cy) {
    int x0 = cx << CHUNK_SHIFT, y0 = cy << CHUNK_SHIFT;
    for (int y = y0; y < y0 + CHUNK_SIZE; y++) {
        const Cell *src = &w->cells[world_index(w, x0, y)];
        uint32_t *dst = &r->pixels[(size_t)y * r->width + x0];
        for (int i = 0; i < CHUNK_SIZE; i++) {
            dst[i] = r->lut[src[i].element][src[i].shade];
        }
    }
}

void render_upload(WorldRenderer *r, World *w) {
    int n = w->chunks_x * w->chunks_y;
    int dirty = 0;
    for (int i = 0; i < n; i++) {
        dirty += w->chunks[i].render_dirty;
    }
    r->chunks_uploaded = (uint32_t)dirty;
    if (dirty == 0) {
        return;
    }
    // Many dirty chunks: one full upload beats many small ones.
    bool full = dirty * 4 > n;
    int pitch = r->width * (int)sizeof(uint32_t);
    for (int cy = 0; cy < w->chunks_y; cy++) {
        for (int cx = 0; cx < w->chunks_x; cx++) {
            Chunk *c = world_chunk(w, cx, cy);
            if (!c->render_dirty) {
                continue;
            }
            c->render_dirty = 0;
            convert_chunk(r, w, cx, cy);
            if (!full) {
                SDL_Rect rect = {cx << CHUNK_SHIFT, cy << CHUNK_SHIFT, CHUNK_SIZE, CHUNK_SIZE};
                const uint32_t *src = &r->pixels[(size_t)rect.y * r->width + rect.x];
                SDL_UpdateTexture(r->texture, &rect, src, pitch);
            }
        }
    }
    if (full) {
        SDL_UpdateTexture(r->texture, NULL, r->pixels, pitch);
    }
}

void render_draw(WorldRenderer *r, const SDL_FRect *dst) {
    SDL_RenderTexture(r->sdl, r->texture, NULL, dst);
}
