#include "game/app.h"

#include "core/log.h"
#include "sim/scene.h"

#include <SDL3/SDL.h>

#include <math.h>

#define TICK_DT             (1.0 / 60.0)
#define MAX_TICKS_PER_FRAME 4
#define PANEL_W             200.0f
#define BRUSH_MAX           256
#define WINDOW_W            1440
#define WINDOW_H            900

typedef struct Tool {
    ElementId element;
    const char *label; // NULL: use the element name
} Tool;

// Toolbar order = number keys 1..9.
static const Tool k_tools[] = {
    {ELEM_WATER, NULL},    {ELEM_OXYGEN, NULL}, {ELEM_CO2, "CO2"},
    {ELEM_HYDROGEN, NULL}, {ELEM_DIRT, NULL},   {ELEM_ROCK, NULL},
    {ELEM_IRON, NULL},     {ELEM_ICE, NULL},    {ELEM_VOID, "Eraser"},
};
#define TOOL_COUNT ((int)PR_ARRAY_COUNT(k_tools))

static void reset_world(App *app) {
    if (app->cfg.demo) {
        scene_demo(app->world);
    } else {
        scene_empty_box(app->world, SCENE_WALL_THICKNESS);
    }
}

bool app_init(App *app, const AppConfig *cfg) {
    *app = (App){0};
    app->cfg = *cfg;
    app->brush_radius = 8;
    app->running = true;

    if (!SDL_CreateWindowAndRenderer("pixel-roguelike", WINDOW_W, WINDOW_H, SDL_WINDOW_RESIZABLE,
                                     &app->window, &app->renderer)) {
        pr_log_error("SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        return false;
    }
    if (cfg->frames <= 0) {
        SDL_SetRenderVSync(app->renderer, 1);
    }
    pr_log_info("Renderer: %s", SDL_GetRendererName(app->renderer));

    app->world = world_create(cfg->world_w, cfg->world_h, cfg->seed);
    if (!app->world) {
        return false;
    }
    reset_world(app);
    if (!render_init(&app->wr, app->renderer, app->world)) {
        return false;
    }
    app->stat_t0 = pr_time_seconds();
    return true;
}

void app_shutdown(App *app) {
    render_shutdown(&app->wr);
    world_destroy(app->world);
    if (app->renderer) {
        SDL_DestroyRenderer(app->renderer);
    }
    if (app->window) {
        SDL_DestroyWindow(app->window);
    }
}

static void screen_to_world(const App *app, float sx, float sy, int *wx, int *wy) {
    const SDL_FRect *d = &app->world_dst;
    *wx = (int)floorf((sx - d->x) * (float)app->world->width / d->w);
    *wy = (int)floorf((sy - d->y) * (float)app->world->height / d->h);
}

static void set_radius(App *app, int r) {
    app->brush_radius = PR_CLAMP(r, 0, BRUSH_MAX);
}

static int radius_step(int r) {
    return PR_MAX(1, r / 8);
}

static void queue_segment(App *app, int wx, int wy) {
    Edit e = {0};
    e.x0 = app->last_wx;
    e.y0 = app->last_wy;
    e.x1 = wx;
    e.y1 = wy;
    e.radius = app->brush_radius;
    e.shape = app->brush_square ? EDIT_SQUARE : EDIT_CIRCLE;
    e.element = app->paint_button == SDL_BUTTON_RIGHT ? ELEM_VOID : k_tools[app->tool].element;
    e.only_void = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0 && e.element != ELEM_VOID;
    if (!edit_queue_push(&app->edits, &e)) {
        sim_apply_edits(app->world, &app->edits);
        edit_queue_push(&app->edits, &e);
    }
    app->last_wx = wx;
    app->last_wy = wy;
    app->stroke_moved = true;
}

// Strokes are built from mouse events, so a release always ends the stroke at the release point
// (never at wherever the cursor goes next). Holding the button still keeps painting each frame.
static void stroke_to(App *app, float sx, float sy) {
    int wx, wy;
    screen_to_world(app, sx, sy, &wx, &wy);
    queue_segment(app, wx, wy);
}

void app_handle_event(App *app, const SDL_Event *ev) {
    switch (ev->type) {
        case SDL_EVENT_QUIT:
            app->running = false;
            break;
        case SDL_EVENT_KEY_DOWN: {
            SDL_Keycode k = ev->key.key;
            if (k >= SDLK_1 && k <= SDLK_9 && (int)(k - SDLK_1) < TOOL_COUNT) {
                app->tool = (int)(k - SDLK_1);
            } else if (k == SDLK_SPACE) {
                app->paused = !app->paused;
            } else if (k == SDLK_PERIOD) {
                app->paused = true;
                app->step_once = true;
            } else if (k == SDLK_RIGHTBRACKET) {
                set_radius(app, app->brush_radius + radius_step(app->brush_radius));
            } else if (k == SDLK_LEFTBRACKET) {
                set_radius(app, app->brush_radius - radius_step(app->brush_radius));
            } else if (k == SDLK_B) {
                app->brush_square = !app->brush_square;
            } else if (k == SDLK_R) {
                reset_world(app);
            } else if (k == SDLK_ESCAPE) {
                app->running = false;
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            int r = app->brush_radius;
            if (ev->wheel.y > 0) {
                set_radius(app, r + radius_step(r));
            } else if (ev->wheel.y < 0) {
                set_radius(app, r - radius_step(r));
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            app->mouse_x = ev->motion.x;
            app->mouse_y = ev->motion.y;
            if (app->paint_button != 0) {
                stroke_to(app, ev->motion.x, ev->motion.y);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            app->mouse_x = ev->button.x;
            app->mouse_y = ev->button.y;
            bool on_panel = ui_point_in(app->panel, ev->button.x, ev->button.y);
            if (ev->button.button == SDL_BUTTON_LEFT && on_panel) {
                app->ui_click = true;
            } else if (!on_panel && app->paint_button == 0 &&
                       (ev->button.button == SDL_BUTTON_LEFT ||
                        ev->button.button == SDL_BUTTON_RIGHT)) {
                app->paint_button = ev->button.button;
                screen_to_world(app, ev->button.x, ev->button.y, &app->last_wx, &app->last_wy);
                stroke_to(app, ev->button.x, ev->button.y);
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (ev->button.button == app->paint_button) {
                stroke_to(app, ev->button.x, ev->button.y);
                app->paint_button = 0;
            }
            break;
        default:
            break;
    }
}

static void layout(App *app) {
    int ww = 0, wh = 0;
    SDL_GetCurrentRenderOutputSize(app->renderer, &ww, &wh);
    app->panel = (SDL_FRect){0, 0, PANEL_W, (float)wh};
    float avail_w = (float)ww - PANEL_W, avail_h = (float)wh;
    float scale = PR_MIN(avail_w / (float)app->world->width, avail_h / (float)app->world->height);
    if (scale >= 1.0f) {
        scale = floorf(scale); // integer scale keeps every pixel the same size
    }
    float dw = (float)app->world->width * scale, dh = (float)app->world->height * scale;
    app->world_dst = (SDL_FRect){PANEL_W + (avail_w - dw) * 0.5f, (avail_h - dh) * 0.5f, dw, dh};
}

static void draw_brush_outline(App *app) {
    if (!ui_point_in(app->world_dst, app->mouse_x, app->mouse_y)) {
        return;
    }
    float scale = app->world_dst.w / (float)app->world->width;
    int wx, wy;
    screen_to_world(app, app->mouse_x, app->mouse_y, &wx, &wy);
    float cx = app->world_dst.x + ((float)wx + 0.5f) * scale;
    float cy = app->world_dst.y + ((float)wy + 0.5f) * scale;
    float r = ((float)app->brush_radius + 0.5f) * scale;
    SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 160);
    if (app->brush_square) {
        SDL_FRect rect = {cx - r, cy - r, 2.0f * r, 2.0f * r};
        SDL_RenderRect(app->renderer, &rect);
        return;
    }
    enum { SEGMENTS = 48 };
    SDL_FPoint pts[SEGMENTS + 1];
    for (int i = 0; i <= SEGMENTS; i++) {
        float a = (float)i * (6.2831853f / SEGMENTS);
        pts[i] = (SDL_FPoint){cx + r * cosf(a), cy + r * sinf(a)};
    }
    SDL_RenderLines(app->renderer, pts, SEGMENTS + 1);
}

static void draw_panel(App *app) {
    Ui *ui = &app->ui;
    ui_begin(ui, app->renderer, app->mouse_x, app->mouse_y, app->ui_click);
    ui_panel(ui, app->panel, 0x16181D);

    float x = 12.0f, y = 14.0f, w = PANEL_W - 24.0f;
    ui_text(ui, x, y, 0xFFFFFF, "pixel-roguelike");
    y += 22.0f;
    ui_text(ui, x, y, 0x9AA0AA, "TOOLS");
    y += 14.0f;
    for (int i = 0; i < TOOL_COUNT; i++) {
        ElementId e = k_tools[i].element;
        char label[48];
        SDL_snprintf(label, sizeof(label), "%d %s", i + 1,
                     k_tools[i].label ? k_tools[i].label : elem_name(e));
        uint32_t swatch = e == ELEM_VOID ? UI_NO_SWATCH : g_elem_defs[e].color;
        if (ui_button(ui, (SDL_FRect){x, y, w, 24.0f}, label, swatch, app->tool == i)) {
            app->tool = i;
        }
        y += 28.0f;
    }

    y += 8.0f;
    ui_text(ui, x, y, 0x9AA0AA, "BRUSH");
    y += 14.0f;
    ui_text(ui, x, y, 0xE8E8E8, "%s, radius %d", app->brush_square ? "Square" : "Circle",
            app->brush_radius);
    y += 22.0f;

    ui_text(ui, x, y, 0x9AA0AA, "STATS");
    y += 14.0f;
    ui_text(ui, x, y, 0xE8E8E8, "FPS      %6.1f", app->fps);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "Sim      %6.2f ms", app->sim_ms);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "Updated  %7llu",
            (unsigned long long)app->last_stats.cells_visited);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "Chunks   %4u/%d", app->last_stats.chunks_active,
            app->world->chunks_x * app->world->chunks_y);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "World    %dx%d", app->world->width, app->world->height);
    y += 12.0f;
    ui_text(ui, x, y, app->paused ? 0xFFC060 : 0xE8E8E8, "%s  tick %llu",
            app->paused ? "PAUSED " : "Running", (unsigned long long)app->world->tick);
    y += 22.0f;

    ui_text(ui, x, y, 0x9AA0AA, "CURSOR");
    y += 14.0f;
    if (ui_point_in(app->world_dst, app->mouse_x, app->mouse_y)) {
        int wx, wy;
        screen_to_world(app, app->mouse_x, app->mouse_y, &wx, &wy);
        ElementId e = world_elem(app->world, wx, wy);
        ui_text(ui, x, y, 0xE8E8E8, "%d, %d", wx, wy);
        y += 12.0f;
        ui_text(ui, x, y, 0xE8E8E8, "%s", elem_name(e));
        y += 12.0f;
        if (e != ELEM_VOID && e != ELEM_OUT) {
            float t = app->world->temp[world_index(app->world, wx, wy)];
            ui_text(ui, x, y, 0xE8E8E8, "%.1f C", (double)(t - 273.15f));
        }
    }

    float hy = app->panel.h - 8.0f * 12.0f - 8.0f;
    const char *help[] = {
        "LMB  paint",
        "RMB  erase",
        "Shift  only into void",
        "Wheel [ ]  brush size",
        "B  square/circle",
        "Space  pause  . step",
        "R  reset   Esc  quit",
    };
    for (int i = 0; i < (int)PR_ARRAY_COUNT(help); i++) {
        ui_text(ui, x, hy + 12.0f * (float)i, 0x7A808A, "%s", help[i]);
    }
}

static void update_stats(App *app) {
    double now = pr_time_seconds();
    app->stat_frames++;
    if (now - app->stat_t0 >= 0.5) {
        double dt = now - app->stat_t0;
        app->fps = app->stat_frames / dt;
        app->sim_ms = app->stat_ticks > 0 ? app->stat_sim_s * 1000.0 / app->stat_ticks : 0.0;
        app->stat_t0 = now;
        app->stat_frames = 0;
        app->stat_ticks = 0;
        app->stat_sim_s = 0.0;
    }
}

static void run_tick(App *app) {
    double t0 = pr_time_seconds();
    sim_tick(app->world, &app->last_stats);
    app->stat_sim_s += pr_time_seconds() - t0;
    app->stat_ticks++;
}

bool app_frame(App *app, double frame_dt) {
    layout(app);
    if (app->paint_button != 0 && !app->stroke_moved) {
        queue_segment(app, app->last_wx, app->last_wy); // button held still: keep pouring
    }
    app->stroke_moved = false;
    sim_apply_edits(app->world, &app->edits); // between ticks, on the main thread

    if (app->cfg.frames > 0) {
        if (!app->paused) {
            run_tick(app); // deterministic: exactly one tick per frame
        }
    } else if (app->paused) {
        app->accumulator = 0.0;
        if (app->step_once) {
            run_tick(app);
        }
    } else {
        app->accumulator += frame_dt;
        int ticks = 0;
        while (app->accumulator >= TICK_DT && ticks < MAX_TICKS_PER_FRAME) {
            run_tick(app);
            app->accumulator -= TICK_DT;
            ticks++;
        }
        if (ticks == MAX_TICKS_PER_FRAME) {
            app->accumulator = 0.0; // too slow: drop the backlog instead of spiralling
        }
    }
    app->step_once = false;

    SDL_SetRenderDrawColor(app->renderer, 8, 8, 10, 255);
    SDL_RenderClear(app->renderer);
    render_upload(&app->wr, app->world);
    render_draw(&app->wr, &app->world_dst);
    draw_brush_outline(app);
    draw_panel(app);
    app->ui_click = false;

    app->frame_count++;
    update_stats(app);
    bool last = app->cfg.frames > 0 && app->frame_count >= (uint64_t)app->cfg.frames;
    if (last && app->cfg.screenshot) {
        SDL_Surface *s = SDL_RenderReadPixels(app->renderer, NULL);
        if (!s || !SDL_SaveBMP(s, app->cfg.screenshot)) {
            pr_log_error("Screenshot failed: %s", SDL_GetError());
        }
        SDL_DestroySurface(s);
    }
    SDL_RenderPresent(app->renderer);
    return app->running && !last;
}
