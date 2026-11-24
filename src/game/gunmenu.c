#include "game/gunmenu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CARD_W      270
#define CARD_H      290
#define CARD_GAP    8
#define CARD_ZOOM   40  // the design as big as this in a card, if it fits
#define CARD_AREA   140 // the height of the drawing in a card
#define EDITOR_ZOOM 50
#define CANVAS_X    40
#define CANVAS_Y    100

void gunmenu_init(GunMenu *m) {
    *m = (GunMenu){0};
    m->tool = TOOL_MATTER;
    m->element = ELEM_IRON;
    m->release = RELEASE_IMPACT;
    m->kpa = BULLET_DEFAULT_KPA;
    m->percent = BULLET_DEFAULT_PERCENT;
    m->temp_c = BULLET_DEFAULT_TEMP_K - 273.15f;
}

bool gunmenu_key(GunMenu *m, SDL_Keycode key) {
    if (key == SDLK_TAB) {
        if (m->state == MENU_CLOSED) {
            m->state = MENU_GUN;
        } else if (m->state == MENU_GUN) {
            m->state = MENU_CLOSED;
        }
        m->left_down = m->right_down = m->click = false;
        return true;
    }
    if (m->state == MENU_CLOSED) {
        return false;
    }
    if (key == SDLK_ESCAPE) {
        m->state = m->state == MENU_EDITOR ? MENU_GUN : MENU_CLOSED;
    }
    return true;
}

void gunmenu_mouse(GunMenu *m, const SDL_Event *ev) {
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (ev->button.button == SDL_BUTTON_LEFT) {
            m->left_down = true;
            m->click = true;
        } else if (ev->button.button == SDL_BUTTON_RIGHT) {
            m->right_down = true;
        }
    } else if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (ev->button.button == SDL_BUTTON_LEFT) {
            m->left_down = false;
        } else if (ev->button.button == SDL_BUTTON_RIGHT) {
            m->right_down = false;
        }
    }
}

// ---- Drawing helpers ----

static void rgb_color(SDL_Renderer *r, uint32_t rgb) {
    SDL_SetRenderDrawColor(r, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 255);
}

// Text with a dark shadow, readable on any colour.
static void label(Ui *ui, float x, float y, uint32_t rgb, const char *text) {
    ui_text(ui, x + 1.0f, y + 1.0f, 0x000000, "%s", text);
    ui_text(ui, x, y, rgb, "%s", text);
}

static uint32_t pixel_color(const BulletPixel *p) {
    if (p->kind == BULLET_HEAT) {
        float c = p->temp_k - 273.15f;
        if (c < 0.0f) {
            return 0x2850FF; // cold
        }
        float g = PR_CLAMP(c / 2500.0f, 0.0f, 1.0f), b = PR_CLAMP((c - 2500.0f) / 2500.0f, 0.0f, 1.0f);
        return 0xFF0000u | ((uint32_t)(g * 255.0f) << 8) | (uint32_t)(b * 255.0f);
    }
    return g_elem_defs[p->element].color;
}

// What a pixel shows: its density (kPa for a gas, kg/m3 for the rest, the temperature for heat) and
// how it is released.
static void pixel_texts(const BulletPixel *p, char *line1, size_t n1, char *line2, size_t n2) {
    if (p->kind == BULLET_HEAT) {
        snprintf(line1, n1, "%.0fC", (double)(p->temp_k - 273.15f));
    } else if (elem_species(p->element) >= 0) {
        snprintf(line1, n1, "%.0fkPa", (double)p->kpa);
    } else {
        snprintf(line1, n1, "%.0fkg", (double)g_elem_defs[p->element].density);
    }
    bool solid = p->kind == BULLET_MATTER && bullet_element_is_solid(p->element);
    if (solid) {
        snprintf(line2, n2, "SOLID");
    } else if (p->release == RELEASE_IMPACT) {
        snprintf(line2, n2, "IMPACT");
    } else {
        snprintf(line2, n2, "G %.1f%%", (double)p->percent);
    }
}

// One line about a pixel for the legend of a card: "Hydrogen 300kPa grad 1%".
static void describe_pixel(const BulletPixel *p, char *out, size_t n) {
    const char *name = p->kind == BULLET_HEAT ? "Heat" : g_elem_defs[p->element].name;
    if (p->kind == BULLET_MATTER && bullet_element_is_solid(p->element)) {
        snprintf(out, n, "%s (solid)", name);
        return;
    }
    char dens[24] = "";
    if (p->kind == BULLET_HEAT) {
        snprintf(dens, sizeof(dens), " %.0fC", (double)(p->temp_k - 273.15f));
    } else if (elem_species(p->element) >= 0) {
        snprintf(dens, sizeof(dens), " %.0fkPa", (double)p->kpa);
    }
    if (p->release == RELEASE_IMPACT) {
        snprintf(out, n, "%s%s impact", name, dens);
    } else {
        snprintf(out, n, "%s%s grad %.3g%%", name, dens, (double)p->percent);
    }
}

// Draws pixel p in the rect: its colour, and (if the pixel is big enough) its numbers.
static void draw_pixel(Ui *ui, SDL_FRect rect, const BulletPixel *p, bool labels) {
    ui_panel(ui, rect, pixel_color(p));
    if (labels && rect.w >= 44.0f) {
        char l1[24], l2[24];
        pixel_texts(p, l1, sizeof(l1), l2, sizeof(l2));
        label(ui, rect.x + 3.0f, rect.y + rect.h * 0.5f - 10.0f, 0xFFFFFF, l1);
        label(ui, rect.x + 3.0f, rect.y + rect.h * 0.5f + 2.0f, 0xFFFFA0, l2);
    }
}

// The bounding box of a design's pixels (in pixels); false if it is empty.
static bool design_box(const BulletDesign *d, int *x0, int *y0, int *x1, int *y1) {
    *x0 = BULLET_DIM;
    *y0 = BULLET_DIM;
    *x1 = -1;
    *y1 = -1;
    for (int i = 0; i < BULLET_PIXELS; i++) {
        if (d->px[i].kind != BULLET_EMPTY) {
            *x0 = PR_MIN(*x0, i % BULLET_DIM);
            *x1 = PR_MAX(*x1, i % BULLET_DIM);
            *y0 = PR_MIN(*y0, i / BULLET_DIM);
            *y1 = PR_MAX(*y1, i / BULLET_DIM);
        }
    }
    return *x1 >= 0;
}

// ---- Small controls ----

static float shift_step(float base) {
    SDL_Keymod mod = SDL_GetModState();
    if (mod & SDL_KMOD_CTRL) {
        return base * 100.0f;
    }
    if (mod & SDL_KMOD_SHIFT) {
        return base * 10.0f;
    }
    return base;
}

// "label  value [-] [+]": returns -1, 0 or +1 (a click on - or +). x, y: the left of the line.
static int stepper(Ui *ui, float x, float y, const char *text) {
    ui_text(ui, x, y + 6.0f, 0xE8E8E8, "%s", text);
    float bx = x + (float)strlen(text) * UI_FONT_SIZE + 12.0f;
    int d = 0;
    if (ui_button(ui, (SDL_FRect){bx, y, 24.0f, 20.0f}, "-", UI_NO_SWATCH, false)) {
        d = -1;
    }
    if (ui_button(ui, (SDL_FRect){bx + 28.0f, y, 24.0f, 20.0f}, "+", UI_NO_SWATCH, false)) {
        d = 1;
    }
    return d;
}

// ---- The gun menu ----

static void gun_menu(GunMenu *m, Ui *ui, int w, int h, Rifle *gun) {
    (void)h;
    label(ui, 24.0f, 14.0f, 0xFFFFFF, "GUN   (Tab or Esc: close)");
    char t[64];
    float y = 40.0f;
    snprintf(t, sizeof(t), "Fire delay %d ticks", gun->fire_interval);
    int d = stepper(ui, 24.0f, y, t);
    gun->fire_interval = PR_CLAMP(gun->fire_interval + d * (int)shift_step(1.0f), 1, 120);
    snprintf(t, sizeof(t), "Reload %d ticks", gun->reload_ticks);
    d = stepper(ui, 330.0f, y, t);
    gun->reload_ticks = PR_CLAMP(gun->reload_ticks + d * (int)shift_step(1.0f), 0, 600);
    snprintf(t, sizeof(t), "Slots %d", (int)gun->nslots);
    d = stepper(ui, 620.0f, y, t);
    if (d > 0 && gun->nslots < GUN_SLOTS_MAX) {
        bullet_design_iron(&gun->slots[gun->nslots++]);
    } else if (d < 0 && gun->nslots > 1) {
        gun->nslots--;
    }
    gun->next_slot = 0;
    gun->reload = 0;
    ui_text(ui, 24.0f, 68.0f, 0x9AA0AA,
            "A shot fires the next slot; after the last one it reloads. (Shift: x10 steps.)");

    int cols = PR_MAX(1, (w - 24) / (CARD_W + CARD_GAP));
    for (int s = 0; s < gun->nslots; s++) {
        float x = 24.0f + (float)(s % cols) * (CARD_W + CARD_GAP);
        float cy = 92.0f + (float)(s / cols) * (CARD_H + CARD_GAP);
        ui_panel(ui, (SDL_FRect){x, cy, CARD_W, CARD_H}, 0x1A1E26);
        snprintf(t, sizeof(t), "SLOT %d", s + 1);
        ui_text(ui, x + 8.0f, cy + 8.0f, 0xE8E8E8, "%s", t);
        const BulletDesign *des = &gun->slots[s];
        int x0, y0, x1, y1;
        if (design_box(des, &x0, &y0, &x1, &y1)) {
            int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
            float area_w = (float)(CARD_W - 16), area_h = (float)CARD_AREA;
            float zoom = PR_MIN((float)CARD_ZOOM,
                                floorf(PR_MIN(area_w / (float)bw, area_h / (float)bh)));
            float ox = x + 8.0f + (area_w - zoom * (float)bw) * 0.5f;
            float oy = cy + 28.0f + (area_h - zoom * (float)bh) * 0.5f;
            for (int py = y0; py <= y1; py++) {
                for (int px = x0; px <= x1; px++) {
                    const BulletPixel *p = &des->px[py * BULLET_DIM + px];
                    if (p->kind != BULLET_EMPTY) {
                        SDL_FRect r = {ox + (float)(px - x0) * zoom, oy + (float)(py - y0) * zoom,
                                       zoom, zoom};
                        draw_pixel(ui, r, p, true);
                    }
                }
            }
        }
        // What each pixel is, in numbers (up to 7 lines).
        int shown = 0;
        for (int i = 0; i < BULLET_PIXELS; i++) {
            const BulletPixel *p = &des->px[i];
            if (p->kind == BULLET_EMPTY) {
                continue;
            }
            if (shown == 7) {
                ui_text(ui, x + 8.0f, cy + 176.0f + (float)shown * 12.0f, 0x9AA0AA, "...");
                break;
            }
            char line[64];
            describe_pixel(p, line, sizeof(line));
            ui_text(ui, x + 8.0f, cy + 176.0f + (float)shown * 12.0f, 0xC8D0E0, "%.32s", line);
            shown++;
        }
        if (ui_button(ui, (SDL_FRect){x + CARD_W - 70.0f, cy + CARD_H - 30.0f, 62.0f, 22.0f}, "Edit",
                      UI_NO_SWATCH, false)) {
            m->slot = s;
            m->design = *des;
            m->state = MENU_EDITOR;
        }
    }
}

// ---- The editor ----

static int palette_group(ElementId e) {
    uint8_t b = g_elem_behavior[e];
    if (b == BEHAVIOR_STATIC) {
        return 0;
    }
    if (b == BEHAVIOR_POWDER) {
        return 1;
    }
    if (b == BEHAVIOR_LIQUID) {
        return 2;
    }
    return elem_species(e) >= 0 ? 3 : -1;
}

static bool tool_is_gas(const GunMenu *m) {
    return m->tool == TOOL_MATTER && elem_species(m->element) >= 0;
}

static bool tool_is_liquid(const GunMenu *m) {
    return m->tool == TOOL_MATTER && g_elem_behavior[m->element] == BEHAVIOR_LIQUID;
}

static void editor(GunMenu *m, Ui *ui, Rifle *gun) {
    char t[96];
    snprintf(t, sizeof(t), "BULLET OF SLOT %d", m->slot + 1);
    label(ui, 24.0f, 14.0f, 0xFFFFFF, t);
    ui_text(ui, 24.0f, 34.0f, 0x9AA0AA, "Left button: draw   Right button: erase   All pixels must touch");
    ui_text(ui, 24.0f, 46.0f, 0x9AA0AA, "Pick an element on the right. Elements are unlimited for now.");

    // The canvas.
    float cs = EDITOR_ZOOM;
    SDL_FRect canvas = {CANVAS_X, CANVAS_Y, cs * BULLET_DIM, cs * BULLET_DIM};
    ui_panel(ui, canvas, 0x0B0B10);
    for (int i = 0; i < BULLET_PIXELS; i++) {
        float px = CANVAS_X + (float)(i % BULLET_DIM) * cs, py = CANVAS_Y + (float)(i / BULLET_DIM) * cs;
        if (m->design.px[i].kind != BULLET_EMPTY) {
            draw_pixel(ui, (SDL_FRect){px, py, cs, cs}, &m->design.px[i], true);
        }
    }
    rgb_color(ui->sdl, 0x2A3040);
    for (int k = 0; k <= BULLET_DIM; k++) {
        SDL_RenderLine(ui->sdl, CANVAS_X + (float)k * cs, CANVAS_Y, CANVAS_X + (float)k * cs,
                       CANVAS_Y + cs * BULLET_DIM);
        SDL_RenderLine(ui->sdl, CANVAS_X, CANVAS_Y + (float)k * cs, CANVAS_X + cs * BULLET_DIM,
                       CANVAS_Y + (float)k * cs);
    }
    // Drawing (the button held over the canvas).
    if (ui_point_in(canvas, ui->mouse_x, ui->mouse_y) && (m->left_down || m->right_down)) {
        int cx = (int)((ui->mouse_x - CANVAS_X) / cs), cy = (int)((ui->mouse_y - CANVAS_Y) / cs);
        BulletPixel *p = &m->design.px[cy * BULLET_DIM + cx];
        if (m->right_down || m->tool == TOOL_ERASE) {
            *p = (BulletPixel){0};
        } else if (m->tool == TOOL_HEAT) {
            *p = (BulletPixel){BULLET_HEAT, ELEM_VOID, (uint8_t)m->release, 0.0f, m->percent,
                               m->temp_c + 273.15f};
        } else {
            bool solid = bullet_element_is_solid(m->element);
            *p = (BulletPixel){BULLET_MATTER, m->element,
                               (uint8_t)(solid ? RELEASE_IMPACT : m->release),
                               tool_is_gas(m) ? m->kpa : 0.0f, m->percent, 0.0f};
        }
    }

    // The palette: every element that can be drawn, by kind.
    static const char *const k_groups[4] = {"SOLIDS", "POWDERS", "LIQUIDS", "GASES"};
    float px0 = 480.0f, py = 92.0f;
    for (int g = 0; g < 4; g++) {
        ui_text(ui, px0, py, 0x9AA0AA, "%s", k_groups[g]);
        py += 14.0f;
        int n = 0;
        for (ElementId e = 1; e < ELEM_COUNT; e++) {
            if (!bullet_element_allowed(e) || palette_group(e) != g) {
                continue;
            }
            SDL_FRect r = {px0 + (float)(n % 3) * 168.0f, py + (float)(n / 3) * 24.0f, 162.0f, 22.0f};
            bool sel = m->tool == TOOL_MATTER && m->element == e;
            if (ui_button(ui, r, g_elem_defs[e].name, g_elem_defs[e].color, sel)) {
                m->tool = TOOL_MATTER;
                m->element = e;
            }
            n++;
        }
        py += (float)((n + 2) / 3) * 24.0f + 8.0f;
    }
    ui_text(ui, px0, py, 0x9AA0AA, "OTHER");
    py += 14.0f;
    if (ui_button(ui, (SDL_FRect){px0, py, 162.0f, 22.0f}, "Temperature", 0xFF6020,
                  m->tool == TOOL_HEAT)) {
        m->tool = TOOL_HEAT;
    }
    if (ui_button(ui, (SDL_FRect){px0 + 168.0f, py, 162.0f, 22.0f}, "Eraser", UI_NO_SWATCH,
                  m->tool == TOOL_ERASE)) {
        m->tool = TOOL_ERASE;
    }

    // The properties of what is drawn next.
    float x = CANVAS_X, y = CANVAS_Y + cs * BULLET_DIM + 24.0f;
    if (m->tool == TOOL_HEAT) {
        label(ui, x, y, 0xFFFFFF, "DRAWING: Temperature");
    } else if (m->tool == TOOL_ERASE) {
        label(ui, x, y, 0xFFFFFF, "DRAWING: Eraser");
    } else {
        snprintf(t, sizeof(t), "DRAWING: %s", g_elem_defs[m->element].name);
        label(ui, x, y, 0xFFFFFF, t);
    }
    y += 18.0f;
    if (tool_is_gas(m)) {
        float limit = gas_max_pressure(m->element, g_elem_defs[m->element].spawn_temp) / 1000.0f;
        snprintf(t, sizeof(t), "Density %.0f kPa", (double)m->kpa);
        int d = stepper(ui, x, y, t);
        m->kpa = PR_CLAMP(m->kpa + (float)d * shift_step(50.0f), 1.0f, limit);
        ui_text(ui, x + 300.0f, y + 6.0f, 0x9AA0AA, "(at most %.0f: it would be liquid)", (double)limit);
        y += 28.0f;
    }
    if (m->tool == TOOL_HEAT) {
        snprintf(t, sizeof(t), "Temperature %.0f C", (double)m->temp_c);
        int d = stepper(ui, x, y, t);
        m->temp_c = PR_CLAMP(m->temp_c + (float)d * shift_step(100.0f), -200.0f, 4700.0f);
        y += 28.0f;
    }
    if (tool_is_gas(m) || tool_is_liquid(m) || m->tool == TOOL_HEAT) {
        ui_text(ui, x, y + 6.0f, 0xE8E8E8, "Release:");
        if (ui_button(ui, (SDL_FRect){x + 80.0f, y, 150.0f, 22.0f}, "Release on impact", UI_NO_SWATCH,
                      m->release == RELEASE_IMPACT)) {
            m->release = RELEASE_IMPACT;
        }
        if (ui_button(ui, (SDL_FRect){x + 236.0f, y, 170.0f, 22.0f}, "Release gradually", UI_NO_SWATCH,
                      m->release == RELEASE_GRADUAL)) {
            m->release = RELEASE_GRADUAL;
        }
        y += 28.0f;
        if (m->release == RELEASE_GRADUAL) {
            snprintf(t, sizeof(t), "Released per pixel traveled %.1f %%", (double)m->percent);
            int d = stepper(ui, x, y, t);
            m->percent = PR_CLAMP(m->percent + (float)d * shift_step(0.5f), 0.5f, 100.0f);
            y += 28.0f;
        }
    } else if (m->tool == TOOL_MATTER) {
        ui_text(ui, x, y + 6.0f, 0x9AA0AA, "Solids are shot as they are and do damage on impact.");
        y += 28.0f;
    }
    ui_text(ui, x, y + 4.0f, 0x7A808A, "(+ and -: Shift x10, Ctrl x100)");

    // Done, or not.
    bool ok = bullet_design_connected(&m->design);
    float by = 700.0f;
    if (!ok) {
        ui_text(ui, 480.0f, by - 18.0f, 0xFF6040,
                bullet_design_pixels(&m->design) == 0 ? "Draw at least one pixel."
                                                      : "All the pixels must touch each other.");
    }
    if (ui_button(ui, (SDL_FRect){480.0f, by, 100.0f, 26.0f}, ok ? "OK" : "OK (not yet)",
                  UI_NO_SWATCH, false) &&
        ok) {
        gun->slots[m->slot] = m->design;
        m->state = MENU_GUN;
    }
    if (ui_button(ui, (SDL_FRect){590.0f, by, 100.0f, 26.0f}, "Cancel", UI_NO_SWATCH, false)) {
        m->state = MENU_GUN;
    }
    if (ui_button(ui, (SDL_FRect){700.0f, by, 100.0f, 26.0f}, "Clear", UI_NO_SWATCH, false)) {
        bullet_design_clear(&m->design);
    }
}

void gunmenu_draw(GunMenu *m, Ui *ui, int w, int h, Rifle *gun) {
    if (m->state == MENU_CLOSED) {
        return;
    }
    SDL_SetRenderDrawBlendMode(ui->sdl, SDL_BLENDMODE_NONE);
    ui_panel(ui, (SDL_FRect){0, 0, (float)w, (float)h}, 0x10131A);
    ui->mouse_pressed = m->click;
    m->click = false;
    if (m->state == MENU_GUN) {
        gun_menu(m, ui, w, h, gun);
    } else {
        editor(m, ui, gun);
    }
}
