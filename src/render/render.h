#pragma once

#include "sim/world.h"

#include <SDL3/SDL_render.h>

typedef struct WorldRenderer {
    SDL_Renderer *sdl;
    SDL_Texture *texture;
    uint32_t *pixels; // world-sized XRGB8888 staging buffer
    int width, height;
    uint32_t lut[ELEM_COUNT][256]; // final colour per (element, shade)
    uint32_t chunks_uploaded;      // last frame, for the HUD
} WorldRenderer;

bool render_init(WorldRenderer *r, SDL_Renderer *sdl, World *w);
void render_shutdown(WorldRenderer *r);

// Converts render-dirty chunks to pixels and uploads them to the texture.
void render_upload(WorldRenderer *r, World *w);

// Draws the world texture into `dst` (window coordinates).
void render_draw(WorldRenderer *r, const SDL_FRect *dst);
