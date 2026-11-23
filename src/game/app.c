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
#define PAN_SPEED           900.0f // screen pixels per second for keyboard panning

// Zoom levels (screen pixels per world cell). Integer levels >= 1 keep every pixel the same size.
static const float k_zoom[] = {0.25f, 1.0f / 3.0f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
#define ZOOM_COUNT ((int)PR_ARRAY_COUNT(k_zoom))
#define ZOOM_ONE   3

// Inspector: partial pressures are averaged over a (2r+1)^2 window around the cursor.
#define INSPECT_RADIUS 7

static const char *const k_gas_short[ELEM_COUNT] = {
    [ELEM_OXYGEN] = "O2", [ELEM_CO2] = "CO2",     [ELEM_HYDROGEN] = "H2",
    [ELEM_STEAM] = "H2O", [ELEM_NITROGEN] = "N2",
};

typedef struct Tool {
    ElementId element;
    const char *label; // NULL: use the element name
    uint8_t mode;      // EditMode
    SDL_Keycode key;
    uint32_t swatch; // UI_NO_SWATCH: none; 0: the element's colour
} Tool;

static const Tool k_tools[] = {
    {ELEM_WATER, NULL, EDIT_PAINT, SDLK_1, 0},
    {ELEM_OXYGEN, NULL, EDIT_PAINT, SDLK_2, 0},
    {ELEM_CO2, "CO2", EDIT_PAINT, SDLK_3, 0},
    {ELEM_HYDROGEN, NULL, EDIT_PAINT, SDLK_4, 0},
    {ELEM_STEAM, NULL, EDIT_PAINT, SDLK_5, 0},
    {ELEM_NITROGEN, NULL, EDIT_PAINT, SDLK_N, 0},
    {EDIT_AIR, "Air", EDIT_PAINT, SDLK_G, 0x8FA4C8},
    {ELEM_DIRT, NULL, EDIT_PAINT, SDLK_6, 0},
    {ELEM_ROCK, NULL, EDIT_PAINT, SDLK_7, 0},
    {ELEM_IRON, NULL, EDIT_PAINT, SDLK_8, 0},
    {ELEM_ICE, NULL, EDIT_PAINT, SDLK_9, 0},
    {ELEM_VOID, "Eraser", EDIT_PAINT, SDLK_0, UI_NO_SWATCH},
    {ELEM_VOID, "Heat", EDIT_HEAT, SDLK_H, 0xFF5020},
    {ELEM_VOID, "Cool", EDIT_COOL, SDLK_C, 0x40A0FF},
};
#define TOOL_COUNT ((int)PR_ARRAY_COUNT(k_tools))

static float zoom(const App *app) {
    return k_zoom[app->zoom_index];
}

static void layout(App *app) {
    int ww = 0, wh = 0;
    SDL_GetCurrentRenderOutputSize(app->renderer, &ww, &wh);
    app->panel = (SDL_FRect){0, 0, PANEL_W, (float)wh};
    app->view = (SDL_FRect){PANEL_W, 0, PR_MAX((float)ww - PANEL_W, 1.0f), PR_MAX((float)wh, 1.0f)};
}

// Keep the view inside the world (or centre the world when it is smaller than the view), and
// snap the camera to whole screen pixels so the image does not shimmer while panning.
static void camera_clamp(App *app) {
    float z = zoom(app);
    float vw = app->view.w / z, vh = app->view.h / z;
    float W = (float)app->world->width, H = (float)app->world->height;
    app->cam_x = W <= vw ? (W - vw) * 0.5f : PR_CLAMP(app->cam_x, 0.0f, W - vw);
    app->cam_y = H <= vh ? (H - vh) * 0.5f : PR_CLAMP(app->cam_y, 0.0f, H - vh);
    app->cam_x = roundf(app->cam_x * z) / z;
    app->cam_y = roundf(app->cam_y * z) / z;
}

// Largest zoom level at which the whole world fits in the view.
static void camera_fit(App *app) {
    app->zoom_index = 0;
    for (int i = ZOOM_COUNT - 1; i >= 0; i--) {
        if ((float)app->world->width * k_zoom[i] <= app->view.w &&
            (float)app->world->height * k_zoom[i] <= app->view.h) {
            app->zoom_index = i;
            break;
        }
    }
    camera_clamp(app);
}

// Change zoom level, keeping the world point under screen position (sx, sy) fixed.
static void camera_zoom_at(App *app, int index, float sx, float sy) {
    index = PR_CLAMP(index, 0, ZOOM_COUNT - 1);
    float wx = app->cam_x + (sx - app->view.x) / zoom(app);
    float wy = app->cam_y + (sy - app->view.y) / zoom(app);
    app->zoom_index = index;
    app->cam_x = wx - (sx - app->view.x) / zoom(app);
    app->cam_y = wy - (sy - app->view.y) / zoom(app);
    camera_clamp(app);
}

static bool mouse_over_world(const App *app) {
    return ui_point_in(app->view, app->mouse_x, app->mouse_y);
}

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
    app->jobs = jobs_create(cfg->threads); // NULL on failure: runs single-threaded
    world_set_job_pool(app->world, app->jobs);
    pr_log_info("World %dx%d, %d simulation threads", app->world->width, app->world->height,
                jobs_thread_count(app->jobs));
    reset_world(app);
    if (!render_init(&app->wr, app->renderer, app->world)) {
        return false;
    }
    layout(app);
    camera_fit(app);
    app->stat_t0 = pr_time_seconds();
    return true;
}

void app_shutdown(App *app) {
    render_shutdown(&app->wr);
    world_destroy(app->world);
    jobs_destroy(app->jobs);
    if (app->renderer) {
        SDL_DestroyRenderer(app->renderer);
    }
    if (app->window) {
        SDL_DestroyWindow(app->window);
    }
}

static void screen_to_world(const App *app, float sx, float sy, int *wx, int *wy) {
    *wx = (int)floorf(app->cam_x + (sx - app->view.x) / zoom(app));
    *wy = (int)floorf(app->cam_y + (sy - app->view.y) / zoom(app));
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
    e.mode = app->paint_button == SDL_BUTTON_RIGHT ? EDIT_PAINT : k_tools[app->tool].mode;
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
            int tool = -1;
            for (int i = 0; i < TOOL_COUNT; i++) {
                if (k_tools[i].key == k) {
                    tool = i;
                }
            }
            if (tool >= 0) {
                app->tool = tool;
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
            } else if (k == SDLK_HOME) {
                camera_fit(app);
            } else if (k == SDLK_P) {
                app->wr.pressure_view = !app->wr.pressure_view;
                app->wr.temperature_view = false;
                render_invalidate(app->world);
            } else if (k == SDLK_T) {
                app->wr.temperature_view = !app->wr.temperature_view;
                app->wr.pressure_view = false;
                render_invalidate(app->world);
            } else if (k == SDLK_ESCAPE) {
                app->running = false;
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            int r = app->brush_radius;
            if (SDL_GetModState() & SDL_KMOD_CTRL) {
                if (ev->wheel.y != 0.0f) {
                    camera_zoom_at(app, app->zoom_index + (ev->wheel.y > 0 ? 1 : -1),
                                   ev->wheel.mouse_x, ev->wheel.mouse_y);
                }
            } else if (ev->wheel.y > 0) {
                set_radius(app, r + radius_step(r));
            } else if (ev->wheel.y < 0) {
                set_radius(app, r - radius_step(r));
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            app->mouse_x = ev->motion.x;
            app->mouse_y = ev->motion.y;
            if (app->panning) {
                app->cam_x -= ev->motion.xrel / zoom(app);
                app->cam_y -= ev->motion.yrel / zoom(app);
                camera_clamp(app);
            }
            if (app->paint_button != 0) {
                stroke_to(app, ev->motion.x, ev->motion.y);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            app->mouse_x = ev->button.x;
            app->mouse_y = ev->button.y;
            bool on_panel = ui_point_in(app->panel, ev->button.x, ev->button.y);
            if (ev->button.button == SDL_BUTTON_MIDDLE && !on_panel) {
                app->panning = true;
            } else if (ev->button.button == SDL_BUTTON_LEFT && on_panel) {
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
            if (ev->button.button == SDL_BUTTON_MIDDLE) {
                app->panning = false;
            }
            if (ev->button.button == app->paint_button) {
                stroke_to(app, ev->button.x, ev->button.y);
                app->paint_button = 0;
            }
            break;
        default:
            break;
    }
}

static void draw_brush_outline(App *app) {
    if (!mouse_over_world(app)) {
        return;
    }
    float scale = zoom(app);
    int wx, wy;
    screen_to_world(app, app->mouse_x, app->mouse_y, &wx, &wy);
    float cx = app->view.x + ((float)wx + 0.5f - app->cam_x) * scale;
    float cy = app->view.y + ((float)wy + 0.5f - app->cam_y) * scale;
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

// Partial pressures of the gases around (wx, wy): each gas's share of the gas-filled cells in a
// window around the cursor, times their pressure (Dalton: the partial pressures add up to the
// total). Two per line, in kPa.
static float draw_partial_pressures(App *app, Ui *ui, float x, float y, int wx, int wy) {
    const int r = INSPECT_RADIUS;
    double p[ELEM_COUNT] = {0};
    int cells = 0;
    for (int yy = wy - r; yy <= wy + r; yy++) {
        for (int xx = wx - r; xx <= wx + r; xx++) {
            ElementId e = world_elem(app->world, xx, yy);
            if (e != ELEM_OUT && g_elem_behavior[e] != BEHAVIOR_STATIC) {
                cells++; // void and fluids: the space gas could fill
            }
            if (g_elem_behavior[e] == BEHAVIOR_GAS) {
                p[e] += (double)world_gas_pressure(app->world, xx, yy);
            }
        }
    }
    if (cells == 0) {
        return y;
    }
    char line[64];
    int len = 0, on_line = 0;
    for (int e = 0; e < ELEM_COUNT; e++) {
        if (p[e] <= 0.0) {
            continue;
        }
        len += SDL_snprintf(line + len, sizeof(line) - (size_t)len, "%s%s %.1f",
                            on_line ? "  " : "", k_gas_short[e], p[e] / cells / 1000.0);
        if (++on_line == 2) {
            ui_text(ui, x, y, 0x9FC8E8, "%s", line);
            y += 12.0f;
            len = on_line = 0;
        }
    }
    if (on_line) {
        ui_text(ui, x, y, 0x9FC8E8, "%s", line);
        y += 12.0f;
    }
    return y;
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
        const Tool *t = &k_tools[i];
        char label[48];
        SDL_snprintf(label, sizeof(label), "%s %s", SDL_GetKeyName(t->key),
                     t->label ? t->label : elem_name(t->element));
        uint32_t swatch = t->swatch ? t->swatch : g_elem_defs[t->element].color;
        if (ui_button(ui, (SDL_FRect){x, y, w, 19.0f}, label, swatch, app->tool == i)) {
            app->tool = i;
        }
        y += 21.0f;
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
    ui_text(ui, x, y, 0xE8E8E8, "Draw     %6.2f ms", app->upload_ms);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "Frame    %6.2f ms", app->work_ms);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "Threads  %6d", jobs_thread_count(app->jobs));
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "Updated  %7llu",
            (unsigned long long)app->last_stats.cells_visited);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "Water    %7u", app->last_stats.transfers);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "Gas      %7u", app->last_stats.gas_transfers);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "Heat     %4u ch", app->last_stats.heat_chunks);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "Chunks   %4u/%d", app->last_stats.chunks_active,
            app->world->chunks_x * app->world->chunks_y);
    y += 12.0f;
    ui_text(ui, x, y, 0xE8E8E8, "World    %dx%d", app->world->width, app->world->height);
    y += 12.0f;
    float z = zoom(app);
    if (z >= 1.0f) {
        ui_text(ui, x, y, 0xE8E8E8, "Zoom     x%d", (int)z);
    } else {
        ui_text(ui, x, y, 0xE8E8E8, "Zoom     x1/%d", (int)(1.0f / z + 0.5f));
    }
    y += 12.0f;
    ui_text(ui, x, y, app->paused ? 0xFFC060 : 0xE8E8E8, "%s  tick %llu",
            app->paused ? "PAUSED " : "Running", (unsigned long long)app->world->tick);
    y += 22.0f;

    ui_text(ui, x, y, 0x9AA0AA, "CURSOR");
    y += 14.0f;
    if (mouse_over_world(app)) {
        int wx, wy;
        screen_to_world(app, app->mouse_x, app->mouse_y, &wx, &wy);
        ElementId e = world_elem(app->world, wx, wy);
        ui_text(ui, x, y, 0xE8E8E8, "%d, %d", wx, wy);
        y += 12.0f;
        ui_text(ui, x, y, 0xE8E8E8, "%s", elem_name(e));
        y += 12.0f;
        if (e != ELEM_VOID && e != ELEM_OUT) {
            float t = app->world->temp[world_index(app->world, wx, wy)];
            ui_text(ui, x, y, 0xE8E8E8, "%.1f C", (double)(elem_temp(e, t) - 273.15f));
            y += 12.0f;
            bool heating = false;
            float progress = elem_phase_progress(e, t, &heating);
            if (progress >= 0.0f) {
                const char *what =
                    heating ? (g_elem_behavior[e] == BEHAVIOR_LIQUID ? "boiling" : "melting")
                            : (g_elem_behavior[e] == BEHAVIOR_GAS ? "condensing" : "freezing");
                ui_text(ui, x, y, 0xFFC060, "%s %d%%", what, (int)(progress * 100.0f));
                y += 12.0f;
            }
            int depth = sim_pressure_depth(app->world, wx, wy);
            if (depth >= 0) {
                // Absolute: gas pressure on the body's surface plus the liquid's weight.
                ui_text(ui, x, y, 0xE8E8E8, "Depth %d  %.1f kPa", depth,
                        (double)sim_liquid_pressure(app->world, wx, wy) / 1000.0);
                y += 12.0f;
            }
            if (g_elem_behavior[e] == BEHAVIOR_GAS) {
                float m = app->world->mass[world_index(app->world, wx, wy)];
                ui_text(ui, x, y, 0xE8E8E8, "%.1f kPa  %.3g mg",
                        (double)world_gas_pressure(app->world, wx, wy) / 1000.0, (double)m * 1e6);
                y += 12.0f;
            }
        }
        y = draw_partial_pressures(app, ui, x, y, wx, wy);
    }

    const char *help[] = {
        "LMB  paint",
        "RMB  erase",
        "Shift  only into void",
        "Wheel [ ]  brush size",
        "B  square/circle",
        "MMB drag, WASD  pan",
        "Ctrl+wheel  zoom",
        "Home  whole world",
        "P  pressure  T  temperature",
        "Space  pause  . step",
        "R  reset   Esc  quit",
    };
    float hy = app->panel.h - (float)PR_ARRAY_COUNT(help) * 12.0f - 8.0f;
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
        app->upload_ms = app->stat_upload_s * 1000.0 / app->stat_frames;
        app->work_ms = app->stat_work_s * 1000.0 / app->stat_frames;
        app->stat_t0 = now;
        app->stat_frames = 0;
        app->stat_ticks = 0;
        app->stat_sim_s = 0.0;
        app->stat_upload_s = 0.0;
        app->stat_work_s = 0.0;
    }
}

static void run_tick(App *app) {
    double t0 = pr_time_seconds();
    sim_tick(app->world, &app->last_stats);
    app->stat_sim_s += pr_time_seconds() - t0;
    app->stat_ticks++;
}

static void pan_with_keys(App *app, double frame_dt) {
    const bool *keys = SDL_GetKeyboardState(NULL);
    float dx = 0.0f, dy = 0.0f;
    dx -= (float)(keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]);
    dx += (float)(keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]);
    dy -= (float)(keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]);
    dy += (float)(keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]);
    if (dx != 0.0f || dy != 0.0f) {
        float step = PAN_SPEED * (float)PR_MIN(frame_dt, 0.1) / zoom(app);
        app->cam_x += dx * step;
        app->cam_y += dy * step;
    }
}

static void draw_world(App *app) {
    float z = zoom(app);
    float W = (float)app->world->width, H = (float)app->world->height;
    // Visible part of the world, and where it goes on screen.
    float x0 = PR_MAX(app->cam_x, 0.0f), y0 = PR_MAX(app->cam_y, 0.0f);
    float x1 = PR_MIN(app->cam_x + app->view.w / z, W);
    float y1 = PR_MIN(app->cam_y + app->view.h / z, H);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    double t0 = pr_time_seconds();
    render_upload(&app->wr, app->world, (int)x0 >> CHUNK_SHIFT, (int)y0 >> CHUNK_SHIFT,
                  ((int)ceilf(x1) - 1) >> CHUNK_SHIFT, ((int)ceilf(y1) - 1) >> CHUNK_SHIFT);
    app->stat_upload_s += pr_time_seconds() - t0;

    SDL_FRect src = {x0, y0, x1 - x0, y1 - y0};
    SDL_FRect dst = {app->view.x + (x0 - app->cam_x) * z, app->view.y + (y0 - app->cam_y) * z,
                     (x1 - x0) * z, (y1 - y0) * z};
    render_draw(&app->wr, &src, &dst, z < 1.0f);
}

bool app_frame(App *app, double frame_dt) {
    double work_t0 = pr_time_seconds();
    layout(app);
    pan_with_keys(app, frame_dt);
    camera_clamp(app);
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
    SDL_Rect clip = {(int)app->view.x, (int)app->view.y, (int)app->view.w, (int)app->view.h};
    SDL_SetRenderClipRect(app->renderer, &clip);
    if (app->wr.pressure_view || app->wr.temperature_view) {
        render_invalidate(app->world); // these can change anywhere: redraw everything
    }
    draw_world(app);
    draw_brush_outline(app);
    SDL_SetRenderClipRect(app->renderer, NULL);
    draw_panel(app);
    app->ui_click = false;
    app->stat_work_s += pr_time_seconds() - work_t0;

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
