#include "ui/ui.h"

#include <stdarg.h>
#include <stdio.h>

static void set_color(SDL_Renderer *r, uint32_t rgb) {
    SDL_SetRenderDrawColor(r, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 255);
}

void ui_begin(Ui *ui, SDL_Renderer *sdl, float mouse_x, float mouse_y, bool mouse_pressed) {
    ui->sdl = sdl;
    ui->mouse_x = mouse_x;
    ui->mouse_y = mouse_y;
    ui->mouse_pressed = mouse_pressed;
}

void ui_panel(Ui *ui, SDL_FRect rect, uint32_t rgb) {
    set_color(ui->sdl, rgb);
    SDL_RenderFillRect(ui->sdl, &rect);
}

bool ui_button(Ui *ui, SDL_FRect rect, const char *label, uint32_t swatch_rgb, bool selected) {
    bool hover = ui_point_in(rect, ui->mouse_x, ui->mouse_y);
    set_color(ui->sdl, selected ? 0x3A4A66 : (hover ? 0x2E3440 : 0x22262E));
    SDL_RenderFillRect(ui->sdl, &rect);
    if (selected) {
        set_color(ui->sdl, 0x88AAEE);
        SDL_RenderRect(ui->sdl, &rect);
    }
    float tx = rect.x + 8.0f;
    if (swatch_rgb < UI_NO_SWATCH) {
        SDL_FRect sw = {rect.x + 6.0f, rect.y + 5.0f, rect.h - 10.0f, rect.h - 10.0f};
        set_color(ui->sdl, swatch_rgb);
        SDL_RenderFillRect(ui->sdl, &sw);
        set_color(ui->sdl, 0x000000);
        SDL_RenderRect(ui->sdl, &sw);
        tx = sw.x + sw.w + 8.0f;
    }
    set_color(ui->sdl, 0xE8E8E8);
    SDL_RenderDebugText(ui->sdl, tx, rect.y + (rect.h - UI_FONT_SIZE) * 0.5f, label);
    return hover && ui->mouse_pressed;
}

void ui_text(Ui *ui, float x, float y, uint32_t rgb, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    set_color(ui->sdl, rgb);
    SDL_RenderDebugText(ui->sdl, x, y, buf);
}
