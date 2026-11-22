#pragma once

#include "core/common.h"

#include <SDL3/SDL_render.h>

// Minimal immediate-mode UI drawn with rects and SDL's built-in 8x8 debug font.
typedef struct Ui {
    SDL_Renderer *sdl;
    float mouse_x, mouse_y;
    bool mouse_pressed; // left button went down this frame
} Ui;

#define UI_FONT_SIZE 8.0f

void ui_begin(Ui *ui, SDL_Renderer *sdl, float mouse_x, float mouse_y, bool mouse_pressed);

void ui_panel(Ui *ui, SDL_FRect rect, uint32_t rgb);

// A button with an optional colour swatch (swatch_rgb >= 0x1000000 means none).
// Returns true when clicked.
bool ui_button(Ui *ui, SDL_FRect rect, const char *label, uint32_t swatch_rgb, bool selected);

void ui_text(Ui *ui, float x, float y, uint32_t rgb, const char *fmt, ...);

static inline bool ui_point_in(SDL_FRect r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

#define UI_NO_SWATCH 0x1000000u
