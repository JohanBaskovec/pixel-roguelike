#pragma once

#include "sim/world.h"

#include <SDL3/SDL_render.h>

typedef struct WorldRenderer {
    SDL_Renderer *sdl;
    SDL_Texture *texture;
    uint32_t *pixels; // world-sized XRGB8888 staging buffer
    int width, height;
    int *dirty_list;               // scratch: chunk indices to convert this frame
    uint32_t lut[ELEM_COUNT][256]; // final colour per (element, shade)
    uint32_t chunks_uploaded;      // last frame, for the HUD
    bool smooth;                   // current texture scale mode
    bool pressure_view;            // colour still liquid by water pressure (depth below its top)
} WorldRenderer;

bool render_init(WorldRenderer *r, SDL_Renderer *sdl, World *w);
void render_shutdown(WorldRenderer *r);

// Marks every chunk for re-render (after a reset, for example).
void render_invalidate(World *w);

// Converts the render-dirty chunks inside the chunk range [cx0..cx1] x [cy0..cy1] (the visible
// ones) to pixels, on the world's job pool, and uploads them: one texture update per chunk row,
// spanning that row's dirty chunks. Dirty chunks outside the range stay dirty until visible.
void render_upload(WorldRenderer *r, World *w, int cx0, int cy0, int cx1, int cy1);

// Draws the part `src` of the world (world cells) into `dst` (window coordinates). `smooth`
// filters (for zoomed-out views); otherwise pixels stay sharp.
void render_draw(WorldRenderer *r, const SDL_FRect *src, const SDL_FRect *dst, bool smooth);
