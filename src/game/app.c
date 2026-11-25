#include "game/app.h"

#include "core/log.h"
#include "sim/creature.h"
#include "sim/enemy.h"
#include "sim/particle.h"
#include "sim/pickup.h"
#include "sim/rigid.h"
#include "sim/save.h"
#include "sim/scene.h"

#include <SDL3/SDL.h>

#include <math.h>

#define TICK_DT             (1.0 / 60.0)
#define MAX_TICKS_PER_FRAME 4 // catching up after a hiccup (a frame that took long)
#define PANEL_W             240.0f
#define BRUSH_MAX           256
#define WINDOW_W            1440
#define WINDOW_H            900
#define PAN_SPEED           900.0f // screen pixels per second for keyboard panning

// Zoom levels (screen pixels per world cell). Integer levels >= 1 keep every pixel the same size.
static const float k_zoom[] = {0.25f, 1.0f / 3.0f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
#define ZOOM_COUNT   ((int)PR_ARRAY_COUNT(k_zoom))
#define ZOOM_DEFAULT 5 // x3: three screen pixels per cell

// Inspector: partial pressures are averaged over a (2r+1)^2 window around the cursor.
#define INSPECT_RADIUS 7

static const char *const k_gas_short[GAS_SPECIES] = {
    [GAS_O2] = "O2", [GAS_CO2] = "CO2", [GAS_H2] = "H2", [GAS_H2O] = "H2O", [GAS_N2] = "N2",
};

typedef struct Tool {
    ElementId element;
    const char *label; // NULL: use the element name
    uint8_t mode;      // EditMode, or TOOL_RIFLE
    SDL_Keycode key;
    uint32_t swatch; // UI_NO_SWATCH: none; 0: the element's colour
} Tool;

#define TOOL_RIFLE 0xFF // the player's rifle (M6a): the left button fires

static const Tool k_tools[] = {
    {ELEM_BULLET, "Rifle", TOOL_RIFLE, SDLK_E, 0x50545C},
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
    {ELEM_SAND, NULL, EDIT_PAINT, SDLK_J, 0},
    {ELEM_LAVA, NULL, EDIT_PAINT, SDLK_L, 0},
    {ELEM_OIL, NULL, EDIT_PAINT, SDLK_O, 0x6A5030},
    {ELEM_WOOD, NULL, EDIT_PAINT, SDLK_V, 0},
    {ELEM_VOID, "Dig", EDIT_DIG, SDLK_X, 0xA07850},
    {ELEM_VOID, "Eraser", EDIT_PAINT, SDLK_0, UI_NO_SWATCH},
    {ELEM_VOID, "Heat", EDIT_HEAT, SDLK_H, 0xFF5020},
    {ELEM_VOID, "Cool", EDIT_COOL, SDLK_C, 0x40A0FF},
};
#define TOOL_COUNT ((int)PR_ARRAY_COUNT(k_tools))

// What the spawn buttons place at the cursor (a click in the world; the right button or a tool
// ends it).
typedef enum SpawnKind {
    SPAWN_NONE = 0,
    SPAWN_WALKER,
    SPAWN_FLYER,
    SPAWN_LOG,
    SPAWN_CRATE,
    SPAWN_COUNT
} SpawnKind;

static const char *const k_spawn_labels[SPAWN_COUNT] = {
    [SPAWN_WALKER] = "Walker", [SPAWN_FLYER] = "Flyer", [SPAWN_LOG] = "Log",
    [SPAWN_CRATE] = "Health crate",
};

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

static Creature *player(App *app) {
    return app->player >= 0 && app->player < app->world->creature_count
               ? &app->world->creatures[app->player]
               : NULL;
}

// The camera centred on the player.
static void camera_follow(App *app) {
    const Creature *c = player(app);
    if (!c || !app->follow) {
        return;
    }
    float z = zoom(app);
    app->cam_x = (float)c->x + (float)c->w * 0.5f - app->view.w / z * 0.5f;
    app->cam_y = (float)c->y + (float)c->h * 0.5f - app->view.h / z * 0.5f;
    camera_clamp(app);
}

static void reset_world(App *app) {
    if (app->cfg.demo) {
        scene_demo(app->world);
    } else {
        scene_level(app->world);
    }
    int sx = app->world->width / 2, sy = SCENE_WALL_THICKNESS;
    if (!app->cfg.demo) {
        scene_level_start(app->world, &sx, &sy);
    }
    app->player = scene_spawn_player_from(app->world, sx, sy);
    rifle_init(&app->rifle);
    app->trigger = false;
    app->follow = true;
}

// ---- The save (D-048): one file, in the user's data folder ----

static const char *save_path(void) {
    static char path[1024];
    if (path[0] == 0) {
        char *dir = SDL_GetPrefPath("pixel-roguelike", "pixel-roguelike");
        SDL_snprintf(path, sizeof(path), "%ssave.bin", dir ? dir : "");
        SDL_free(dir);
    }
    return path;
}

static void toast(App *app, const char *text) {
    SDL_snprintf(app->toast, sizeof(app->toast), "%s", text);
    app->toast_until = pr_time_seconds() + 2.5;
}

static void screen_to_world(const App *app, float sx, float sy, int *wx, int *wy);

// Places what the spawn button chose, centred on cell (wx, wy).
static void spawn_at(App *app, int wx, int wy) {
    World *w = app->world;
    bool ok = false;
    switch (app->spawn) {
        case SPAWN_WALKER:
            ok = enemy_spawn(w, KIND_WALKER, wx - WALKER_W / 2, wy - WALKER_H / 2) >= 0;
            break;
        case SPAWN_FLYER:
            ok = enemy_spawn(w, KIND_FLYER, wx - FLYER_W / 2, wy - FLYER_H / 2) >= 0;
            break;
        case SPAWN_LOG:
            ok = rigid_spawn_box(w, (float)wx + 0.5f, (float)wy + 0.5f, 40, 5, 0.0f) >= 0;
            break;
        case SPAWN_CRATE:
            ok = pickup_add(w, PICKUP_HEALTH, wx, wy + PICKUP_H / 2) >= 0;
            break;
        default:
            break;
    }
    if (ok) {
        world_sound(w, SOUND_SPAWN, wx, wy, 0.8f);
    } else {
        toast(app, "No room there");
    }
}

// Writes the game to the save (to a temporary file first, so a failure keeps the old save).
static bool save_game(App *app) {
    char tmp[1100];
    SDL_snprintf(tmp, sizeof(tmp), "%s.tmp", save_path());
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        pr_log_error("Cannot write %s", tmp);
        toast(app, "Save failed");
        return false;
    }
    SaveExtra extra = {app->player, app->rifle};
    bool ok = world_save(app->world, f, &extra);
    ok = fclose(f) == 0 && ok && SDL_RenamePath(tmp, save_path());
    if (!ok) {
        pr_log_error("Saving to %s failed", save_path());
        SDL_RemovePath(tmp);
    }
    toast(app, ok ? "Saved" : "Save failed");
    return ok;
}

// Loads the save, if there is one of this build. On failure the world is empty: the caller
// builds a level.
static bool load_game(App *app) {
    FILE *f = fopen(save_path(), "rb");
    if (!f) {
        return false;
    }
    SaveExtra extra;
    memset(&extra, 0, sizeof(extra));
    bool ok = world_load(app->world, f, &extra);
    fclose(f);
    if (!ok) {
        pr_log_error("%s is not a save of this build: ignored", save_path());
        return false;
    }
    app->player = extra.player;
    app->rifle = extra.rifle;
    app->trigger = false;
    app->follow = true;
    render_invalidate(app->world);
    toast(app, "Loaded");
    return true;
}

bool app_init(App *app, const AppConfig *cfg) {
    *app = (App){0};
    app->cfg = *cfg;
    app->player = -1;
    app->brush_radius = 8;
    app->gas_atm = 1.0f;
    gunmenu_init(&app->menu);
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
    if (cfg->frames == 0 && !cfg->demo && !cfg->no_audio) {
        audio_init(&app->audio); // without a device the game is silent
    }
    app->saves = !cfg->no_save && cfg->frames == 0 && !cfg->demo;
    if (!(app->saves && !cfg->new_game && load_game(app))) {
        reset_world(app);
    }
    if (cfg->example_bullet) {
        // The example: a pixel of hydrogen and one of oxygen at 300 kPa, released 1% per pixel
        // traveled, and 2000 degC released on impact.
        BulletDesign *d = &app->rifle.slots[1];
        bullet_design_clear(d);
        d->px[0] = (BulletPixel){BULLET_MATTER, ELEM_HYDROGEN, RELEASE_GRADUAL, 300.0f, 1.0f, 0.0f};
        d->px[1] = (BulletPixel){BULLET_MATTER, ELEM_OXYGEN, RELEASE_GRADUAL, 300.0f, 1.0f, 0.0f};
        d->px[2] = (BulletPixel){BULLET_HEAT, ELEM_VOID, RELEASE_IMPACT, 0.0f, 0.0f, 2273.15f};
    }
    if (cfg->menu != 0) {
        app->menu.state = cfg->menu == 2 ? MENU_EDITOR : MENU_GUN;
        app->menu.slot = 1;
        app->menu.design = app->rifle.slots[1];
    }
    if (!render_init(&app->wr, app->renderer, app->world)) {
        return false;
    }
    layout(app);
    app->zoom_index = ZOOM_DEFAULT;
    if (player(app)) {
        camera_follow(app);
    } else {
        camera_fit(app);
    }
    app->stat_t0 = pr_time_seconds();
    return true;
}

void app_shutdown(App *app) {
    const Creature *pl = player(app);
    if (app->saves && pl && pl->state != CREATURE_DEAD) {
        save_game(app); // (not a dead player: the last save is kept)
    }
    audio_shutdown(&app->audio);
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
    e.pressure = app->gas_atm;
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
    // The gun menu takes the keys and the mouse while it is open.
    if (ev->type == SDL_EVENT_KEY_DOWN && gunmenu_key(&app->menu, ev->key.key)) {
        app->trigger = false;
        app->paint_button = 0;
        return;
    }
    if (gunmenu_open(&app->menu)) {
        if (ev->type == SDL_EVENT_MOUSE_MOTION) {
            app->mouse_x = ev->motion.x;
            app->mouse_y = ev->motion.y;
        } else if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev->type == SDL_EVENT_MOUSE_BUTTON_UP) {
            app->mouse_x = ev->button.x;
            app->mouse_y = ev->button.y;
        } else if (ev->type == SDL_EVENT_QUIT) {
            app->running = false;
        }
        gunmenu_mouse(&app->menu, ev);
        return;
    }
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
                app->spawn = SPAWN_NONE;
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
                rifle_reload(&app->rifle);
            } else if (k == SDLK_F6 && app->saves) {
                save_game(app);
            } else if (k == SDLK_F9 && app->saves) {
                if (load_game(app)) {
                    camera_follow(app);
                } else {
                    reset_world(app);
                    toast(app, "No save to load");
                }
            } else if (k == SDLK_F5) {
                reset_world(app);
                camera_follow(app);
            } else if (k == SDLK_HOME) {
                app->follow = false;
                camera_fit(app);
            } else if (k == SDLK_F) {
                app->follow = true;
            } else if (k == SDLK_P) {
                app->wr.pressure_view = !app->wr.pressure_view;
                app->wr.temperature_view = false;
                render_invalidate(app->world);
            } else if (k == SDLK_T) {
                app->wr.temperature_view = !app->wr.temperature_view;
                app->wr.pressure_view = false;
                render_invalidate(app->world);
            } else if (k == SDLK_K && mouse_over_world(app)) {
                // A test creature that wanders, centred on the cursor (M4c).
                int wx, wy;
                screen_to_world(app, app->mouse_x, app->mouse_y, &wx, &wy);
                int id = creature_spawn(app->world, wx - CREATURE_W / 2, wy - CREATURE_H / 2);
                if (id >= 0) {
                    app->world->creatures[id].wander = 1;
                    world_sound(app->world, SOUND_SPAWN, wx, wy, 0.8f);
                }
            } else if (k == SDLK_I && mouse_over_world(app)) {
                // A log (a rigid body, 2 m by 25 cm) centred on the cursor (D-045).
                int wx, wy;
                screen_to_world(app, app->mouse_x, app->mouse_y, &wx, &wy);
                rigid_spawn_box(app->world, (float)wx + 0.5f, (float)wy + 0.5f, 40, 5, 0.0f);
                world_sound(app->world, SOUND_SPAWN, wx, wy, 0.8f);
            } else if (k == SDLK_M) {
                app->audio.muted = !app->audio.muted;
                toast(app, app->audio.muted ? "Sound off" : "Sound on");
            } else if (k == SDLK_MINUS) {
                app->gas_atm = PR_MAX(app->gas_atm * 0.5f, 1.0f / 16.0f); // gas brush: less dense
            } else if (k == SDLK_EQUALS || k == SDLK_PLUS) {
                app->gas_atm = PR_MIN(app->gas_atm * 2.0f, 4096.0f); // denser (up to the liquid's)
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
                app->follow = false;
            } else if (ev->button.button == SDL_BUTTON_LEFT && on_panel) {
                app->ui_click = true;
                audio_play_ui(&app->audio, SOUND_CLICK);
            } else if (app->spawn != SPAWN_NONE && !on_panel) {
                if (ev->button.button == SDL_BUTTON_LEFT) {
                    int wx, wy;
                    screen_to_world(app, ev->button.x, ev->button.y, &wx, &wy);
                    spawn_at(app, wx, wy);
                } else if (ev->button.button == SDL_BUTTON_RIGHT) {
                    app->spawn = SPAWN_NONE;
                }
            } else if (k_tools[app->tool].mode == TOOL_RIFLE) {
                app->trigger = !on_panel && ev->button.button == SDL_BUTTON_LEFT;
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
            if (ev->button.button == SDL_BUTTON_LEFT) {
                app->trigger = false;
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
    if (!mouse_over_world(app) || (k_tools[app->tool].mode == TOOL_RIFLE && !app->spawn)) {
        return;
    }
    float scale = zoom(app);
    int wx, wy;
    screen_to_world(app, app->mouse_x, app->mouse_y, &wx, &wy);
    if (app->spawn != SPAWN_NONE) {
        // The box of what a click places.
        static const int sz[SPAWN_COUNT][2] = {{0, 0}, {WALKER_W, WALKER_H}, {FLYER_W, FLYER_H},
                                               {40, 5}, {PICKUP_W, PICKUP_H}};
        SDL_FRect box = {app->view.x + ((float)(wx - sz[app->spawn][0] / 2) - app->cam_x) * scale,
                         app->view.y + ((float)(wy - sz[app->spawn][1] / 2) - app->cam_y) * scale,
                         (float)sz[app->spawn][0] * scale, (float)sz[app->spawn][1] * scale};
        SDL_SetRenderDrawColor(app->renderer, 255, 255, 120, 220);
        SDL_RenderRect(app->renderer, &box);
        return;
    }
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

// Partial pressures of the gases around (wx, wy), averaged over the cells gas could fill in a
// window around the cursor (Dalton: they add up to the total). Two per line, in kPa.
static float draw_partial_pressures(App *app, Ui *ui, float x, float y, int wx, int wy) {
    const int r = INSPECT_RADIUS;
    double p[GAS_SPECIES] = {0};
    int cells = 0;
    for (int yy = wy - r; yy <= wy + r; yy++) {
        for (int xx = wx - r; xx <= wx + r; xx++) {
            ElementId e = world_elem(app->world, xx, yy);
            if (e != ELEM_OUT && g_elem_behavior[e] != BEHAVIOR_STATIC) {
                cells++; // void and fluids: the space gas could fill
            }
            if (e == ELEM_GAS) {
                for (int s = 0; s < GAS_SPECIES; s++) {
                    p[s] += (double)world_gas_partial_pressure(app->world, xx, yy, s);
                }
            }
        }
    }
    if (cells == 0) {
        return y;
    }
    char line[64];
    int len = 0, on_line = 0;
    for (int s = 0; s < GAS_SPECIES; s++) {
        if (p[s] / cells < 0.05) { // below 0.05 Pa
            continue;
        }
        double kpa = p[s] / cells / 1000.0;
        len += SDL_snprintf(line + len, sizeof(line) - (size_t)len,
                            kpa < 1.0 ? "%s%s %.3f" : "%s%s %.1f", on_line ? "  " : "",
                            k_gas_short[s], kpa);
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
        // Two columns.
        float bw = (w - 4.0f) * 0.5f, bx = x + (float)(i % 2) * (bw + 4.0f);
        if (ui_button(ui, (SDL_FRect){bx, y, bw, 19.0f}, label, swatch, app->tool == i)) {
            app->tool = i;
            app->spawn = SPAWN_NONE;
        }
        if (i % 2 == 1 || i == TOOL_COUNT - 1) {
            y += 21.0f;
        }
    }

    y += 8.0f;
    ui_text(ui, x, y, 0x9AA0AA, "SPAWN (click in the world)");
    y += 14.0f;
    for (int i = SPAWN_WALKER; i < SPAWN_COUNT; i++) {
        float bw = (w - 4.0f) * 0.5f, bx = x + (float)((i - 1) % 2) * (bw + 4.0f);
        if (ui_button(ui, (SDL_FRect){bx, y, bw, 19.0f}, k_spawn_labels[i], UI_NO_SWATCH,
                      app->spawn == i)) {
            app->spawn = app->spawn == i ? SPAWN_NONE : i;
        }
        if ((i - 1) % 2 == 1 || i == SPAWN_COUNT - 1) {
            y += 21.0f;
        }
    }

    y += 8.0f;
    ui_text(ui, x, y, 0x9AA0AA, "BRUSH");
    y += 14.0f;
    ui_text(ui, x, y, 0xE8E8E8, "%s, radius %d", app->brush_square ? "Square" : "Circle",
            app->brush_radius);
    ElementId te = k_tools[app->tool].element;
    if (k_tools[app->tool].mode == EDIT_PAINT && (te == EDIT_AIR || elem_species(te) >= 0)) {
        y += 12.0f;
        float limit = gas_max_pressure(te == EDIT_AIR ? ELEM_NITROGEN : te,
                                       g_elem_defs[te == EDIT_AIR ? ELEM_GAS : te].spawn_temp) /
                      P_ATM;
        ui_text(ui, x, y, 0xE8E8E8, "Gas %.3g atm (max %.3g)", (double)PR_MIN(app->gas_atm, limit),
                (double)limit);
    }
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
    ui_text(ui, x, y, 0xE8E8E8, "Flying   %7u", app->last_stats.particles);
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
            if (e == ELEM_GAS) {
                float m = gas_total(&app->world->gas[world_index(app->world, wx, wy)]);
                ui_text(ui, x, y, 0xE8E8E8, "%.1f kPa  %.3g mg",
                        (double)world_gas_pressure(app->world, wx, wy) / 1000.0, (double)m * 1e6);
                y += 12.0f;
            }
        }
        y = draw_partial_pressures(app, ui, x, y, wx, wy);
    }

    if (player(app)) {
        const Creature *c = player(app);
        static const char *const k_pstate[] = {"ok", "impaired", "DANGER", "dead"};
        y += 10.0f;
        ui_text(ui, x, y, 0x9AA0AA, "PLAYER");
        y += 14.0f;
        ui_text(ui, x, y, 0xE8E8E8, "%s air %3d%%  shots %u", k_pstate[c->state],
                (int)(c->health * 100.0f + 0.5f), app->rifle.shots);
        y += 12.0f;
    }

    // Creatures (M4c): the first few, with the air around their heads.
    if (app->world->creature_count > 0) {
        y += 10.0f;
        ui_text(ui, x, y, 0x9AA0AA, "CREATURES %d", app->world->creature_count);
        y += 14.0f;
        static const char *const k_state[] = {"ok", "impaired", "DANGER", "dead"};
        static const uint32_t k_color[] = {0xE8E8E8, 0xFFC060, 0xFF6040, 0x808080};
        for (int i = 0; i < app->world->creature_count && i < 3; i++) {
            const Creature *c = &app->world->creatures[i];
            ui_text(ui, x, y, k_color[c->state], "#%d %s %3d%%", i + 1, k_state[c->state],
                    (int)(c->health * 100.0f + 0.5f));
            y += 12.0f;
            ui_text(ui, x, y, 0x9FC8E8, " O2 %.1f CO2 %.1f kPa", (double)c->po2 / 1000.0,
                    (double)c->pco2 / 1000.0);
            y += 12.0f;
        }
    }

    const char *help[] = {
        "A D  run  W  jump, hold: jetpack",
        "Tab  gun menu: design bullets",
        "LMB  shoot (Rifle) / paint",
        "RMB  erase",
        "Shift  only into void",
        "Wheel [ ]  brush size",
        "B square/circle  - = gas atm",
        "MMB drag, arrows  pan",
        "F  follow player",
        "Ctrl+wheel  zoom",
        "Home  whole world",
        "P  pressure  T  temperature",
        "K creature  I log at cursor",
        "Space  pause  . step",
        "R reload F5 new F6 save F9 load",
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

// The mouse position in world cells (not rounded).
static void mouse_world(const App *app, float *x, float *y) {
    *x = app->cam_x + (app->mouse_x - app->view.x) / zoom(app);
    *y = app->cam_y + (app->mouse_y - app->view.y) / zoom(app);
}

// Player input for one tick (between ticks, on the main thread): run with A/D, jump with W (hold it for the jetpack)
// (physical keys: Q/D/Z on AZERTY), face the mouse, and fire the rifle while the trigger is held.
static void control_player(App *app) {
    Creature *c = player(app);
    if (!c) {
        return;
    }
    const bool *keys = SDL_GetKeyboardState(NULL);
    c->walk = (int8_t)((int)keys[SDL_SCANCODE_D] - (int)keys[SDL_SCANCODE_A]);
    c->jump = keys[SDL_SCANCODE_W];
    c->thrust = keys[SDL_SCANCODE_W]; // held: the jetpack
    float ax, ay, sx, sy;
    mouse_world(app, &ax, &ay);
    rifle_shoulder(c, &sx, &sy);
    c->facing = ax >= sx ? 1 : -1;
    rifle_update(app->world, &app->rifle, app->player, ax, ay,
                 app->trigger && k_tools[app->tool].mode == TOOL_RIFLE);
}

static void run_tick(App *app) {
    double t0 = pr_time_seconds();
    control_player(app);
    // Only the area around the player is simulated (D-040); without a player, all.
    const Creature *c = player(app);
    pickups_collect(app->world, app->player);
    app->world->view_w = (int)(app->view.w / zoom(app)); // what monsters can see (enemy.c)
    app->world->view_h = (int)(app->view.h / zoom(app));
    if (c) {
        world_simulate_near(app->world, c->x + c->w / 2, c->y + c->h / 2);
    } else {
        world_simulate_all(app->world);
    }
    sim_tick(app->world, &app->last_stats);
    app->stat_sim_s += pr_time_seconds() - t0;
    app->stat_ticks++;
}

static void pan_with_keys(App *app, double frame_dt) {
    const bool *keys = SDL_GetKeyboardState(NULL);
    bool wasd = !player(app); // with a player, WASD moves it
    float dx = 0.0f, dy = 0.0f;
    dx -= (float)((wasd && keys[SDL_SCANCODE_A]) || keys[SDL_SCANCODE_LEFT]);
    dx += (float)((wasd && keys[SDL_SCANCODE_D]) || keys[SDL_SCANCODE_RIGHT]);
    dy -= (float)((wasd && keys[SDL_SCANCODE_W]) || keys[SDL_SCANCODE_UP]);
    dy += (float)((wasd && keys[SDL_SCANCODE_S]) || keys[SDL_SCANCODE_DOWN]);
    if (dx != 0.0f || dy != 0.0f) {
        app->follow = false;
        float step = PAN_SPEED * (float)PR_MIN(frame_dt, 0.1) / zoom(app);
        app->cam_x += dx * step;
        app->cam_y += dy * step;
    }
}

static void world_to_screen(const App *app, float wx, float wy, float *sx, float *sy) {
    *sx = app->view.x + (wx - app->cam_x) * zoom(app);
    *sy = app->view.y + (wy - app->cam_y) * zoom(app);
}

// A line `thick` screen pixels wide (several 1-pixel lines side by side).
static void thick_line(SDL_Renderer *r, float x0, float y0, float x1, float y1, float thick) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) {
        return;
    }
    float nx = -dy / len, ny = dx / len;
    int n = PR_MAX(1, (int)(thick + 0.5f));
    for (int k = 0; k < n; k++) {
        float o = (float)k - (float)(n - 1) * 0.5f;
        SDL_RenderLine(r, x0 + nx * o, y0 + ny * o, x1 + nx * o, y1 + ny * o);
    }
}

// The player's rifle (drawn over the world: it is not made of cells), and a faint streak behind
// each bullet in flight over the last tick (a bullet crosses the screen in a few ticks).
static void draw_rifle_and_tracers(App *app) {
    float z = zoom(app);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    const Creature *c = player(app);
    if (c && c->state != CREATURE_DEAD) {
        float sx, sy, ax, ay;
        rifle_shoulder(c, &sx, &sy);
        mouse_world(app, &ax, &ay);
        float dx = ax - sx, dy = ay - sy, len = sqrtf(dx * dx + dy * dy);
        if (len < 1e-3f) {
            dx = (float)c->facing;
            dy = 0.0f;
            len = 1.0f;
        }
        float ex = sx + dx / len * RIFLE_LENGTH, ey = sy + dy / len * RIFLE_LENGTH;
        float x0, y0, x1, y1;
        world_to_screen(app, sx, sy, &x0, &y0);
        world_to_screen(app, ex, ey, &x1, &y1);
        SDL_SetRenderDrawColor(app->renderer, 120, 124, 132, 255); // gunmetal
        thick_line(app->renderer, x0, y0, x1, y1, PR_MAX(z * 1.5f, 1.0f));
    }
    SDL_SetRenderDrawColor(app->renderer, 255, 220, 150, 110);
    for (int i = 0; i < app->world->particle_count; i++) {
        const Particle *p = &app->world->particles[i];
        if (p->cell.element != ELEM_BULLET || fabsf(p->vx) + fabsf(p->vy) < 20.0f) {
            continue;
        }
        float x0, y0, x1, y1;
        world_to_screen(app, p->x - p->vx, p->y - p->vy, &x0, &y0);
        world_to_screen(app, p->x, p->y, &x1, &y1);
        thick_line(app->renderer, x0, y0, x1, y1, PR_MAX(z, 1.0f));
    }
}

// Crates, the sprites of the player and the monsters (over their body cells), and below the HP
// and ammunition of the player.
static void fill_world_rect(App *app, float x, float y, float w, float h, uint32_t rgb) {
    float x0, y0, x1, y1;
    world_to_screen(app, x, y, &x0, &y0);
    world_to_screen(app, x + w, y + h, &x1, &y1);
    SDL_SetRenderDrawColor(app->renderer, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb,
                           255);
    SDL_FRect r = {x0, y0, PR_MAX(x1 - x0, 1.0f), PR_MAX(y1 - y0, 1.0f)};
    SDL_RenderFillRect(app->renderer, &r);
}

// ---- Sprites (very simple, animated; drawn over the body cells: a colour for each cell) ----
//
// A sprite is a few rows of characters, one per cell of the body, facing right (mirrored when
// it faces left); each character is a colour of sprite_color(). They are only drawing: what the
// simulation sees is the body's cells. A monster is drawn only where it still has a pixel.

static uint32_t sprite_color(char ch) {
    switch (ch) {
        case 'h': return 0x4A3320; // hair
        case 's': return 0xE0AC80; // skin
        case 'e': return 0xFFFFFF; // eye
        case 'p': return 0xD02020; // pupil
        case 'j': return 0x3C6EA8; // jacket
        case 'J': return 0x2C527E;
        case 'b': return 0x6A5A32; // belt
        case 't': return 0x555B6A; // trousers
        case 'k': return 0x26262C; // boots
        case 'g': return 0x4FA04A; // walker
        case 'G': return 0x2F6B2C;
        case 'v': return 0x9A4AB0; // flyer
        case 'V': return 0x6A2A80;
        case 'w': return 0xD9B0E8; // wing
        default: return 0x1E2433;  // 'd' and '.': shadow, nothing there (like the air behind)
    }
}

// The player, 5 x 17 (head and torso, then four poses of the legs).
static const char *const k_player_top[11] = {
    "hhhhh", "hhsss", "hsses", "hssss", "jsssj", "jjjjj",
    "JjjjJ", "JjjjJ", "JjjjJ", "bbbbb", "ttttt",
};
static const char *const k_player_legs[4][6] = {
    {"ttdtt", "ttdtt", "ttdtt", "ttdtt", "ttdtt", "kkdkk"}, // standing, apart
    {"ttdtt", "ttdtt", "ttdtt", "kkdtt", "dddtt", "dddkk"}, // left leg lifted
    {"ttdtt", "ttdtt", "ttdtt", "ttdkk", "ttddd", "kkddd"}, // right leg lifted
    {"ttdtt", "ttdtt", "kkdkk", "ddddd", "ddddd", "ddddd"}, // in the air, knees up
};

// The walker, 6 x 6: two poses of the legs.
static const char *const k_walker[2][6] = {
    {"gggggg", "gggepg", "gggggg", "GGGGGG", "ggddgg", "ggddgg"},
    {"gggggg", "gggepg", "gggggg", "GGGGGG", "dggggd", "dggggd"},
};

// The flyer, 11 x 11: its wings up, level and down.
static const char *const k_flyer[3][11] = {
    {"w.........w", "ww.......ww", "www.VVV.www", ".wwvvvvvww.", "..wvvvvvw..", "...vvvepv..",
     "...vvvvv...", "...VvvvV...", "....VvV....", ".....V.....", "..........."},
    {"....VVV....", "...vvvvv...", "...vvvvv...", "...vvvvv...", "wwwvvvvvwww", "wwwvvvepvww",
     ".wwvvvvvww.", "...VvvvV...", "....VvV....", ".....V.....", "..........."},
    {"....VVV....", "...vvvvv...", "...vvvvv...", "...vvvvv...", "...vvvvv...", "...vvvepv..",
     "..wvvvvvw..", ".wwVvvvVww.", "ww..VvV..ww", "w....V....w", "w.........w"},
};

// Draws a sprite over the w x h cells with top-left cell (x0, y0): mirrored if `mirror`, in white
// where `flash`, dimmed if `dead`; only on cells that hold a body cell if `only_body`.
static void draw_sprite(App *app, const char *const *rows, int w, int h, int x0, int y0,
                        bool mirror, bool flash, bool dead, bool only_body) {
    for (int r = 0; r < h; r++) {
        for (int k = 0; k < w; k++) {
            if (only_body && world_elem(app->world, x0 + k, y0 + r) != ELEM_BODY) {
                continue;
            }
            char ch = rows[r][mirror ? w - 1 - k : k];
            uint32_t rgb = sprite_color(ch);
            if (flash && ch != '.' && ch != 'd') {
                rgb = 0xFFFFFF;
            }
            if (dead) {
                rgb = (rgb >> 1) & 0x7F7F7F;
            }
            fill_world_rect(app, (float)(x0 + k), (float)(y0 + r), 1.0f, 1.0f, rgb);
        }
    }
}

// The player: head, torso and legs (walking by the distance run, so the feet do not slide), and
// the jetpack's flame under the feet.
static void draw_player_sprite(App *app) {
    const Creature *c = player(app);
    if (!c) {
        return;
    }
    const char *rows[17];
    bool moving = c->walk != 0 && c->grounded;
    static const int8_t k_cycle[4] = {0, 1, 0, 2};
    int legs = !c->grounded ? 3 : (moving ? k_cycle[(c->x / 4) & 3] : 0);
    for (int r = 0; r < 11; r++) {
        rows[r] = k_player_top[r];
    }
    for (int r = 0; r < 6; r++) {
        rows[11 + r] = k_player_legs[legs][r];
    }
    draw_sprite(app, rows, c->w, c->h, c->x, c->y, c->facing < 0, c->hit_flash > 0,
                c->state == CREATURE_DEAD, false);
    if (c->state != CREATURE_DEAD && !c->grounded && c->jet_hold >= JETPACK_DELAY) {
        // Flame: a cell wide at the middle of the feet, two to four cells long, flickering.
        int len = 2 + (int)((app->world->tick >> 1) & 1u) + (int)((app->world->tick >> 2) & 1u);
        float cx = (float)c->x + (float)c->w * 0.5f - 1.0f, top = (float)(c->y + c->h);
        fill_world_rect(app, cx, top, 2.0f, 1.0f, 0xFFE070);
        fill_world_rect(app, cx, top + 1.0f, 2.0f, (float)(len - 1), 0xFF8030);
    }
}

static void draw_actors(App *app) {
    const World *w = app->world;
    for (int i = 0; i < w->pickup_count; i++) {
        const Pickup *p = &w->pickups[i];
        if (p->taken) {
            continue;
        }
        {
            fill_world_rect(app, (float)p->x, (float)p->y, (float)p->w, (float)p->h, 0xF0F0F0);
            fill_world_rect(app, (float)p->x + 1, (float)p->y + 1, (float)p->w - 2,
                            (float)p->h - 2, 0xD02828);
            float cx = (float)p->x + (float)p->w * 0.5f, cy = (float)p->y + (float)p->h * 0.5f;
            fill_world_rect(app, cx - 3.0f, cy - 0.5f, 6.0f, 1.0f, 0xFFFFFF);
            fill_world_rect(app, cx - 0.5f, cy - 2.0f, 1.0f, 4.0f, 0xFFFFFF);
        }
    }
    for (int i = 0; i < w->creature_count; i++) {
        const Creature *c = &w->creatures[i];
        if ((c->kind != KIND_WALKER && c->kind != KIND_FLYER) || c->corpse) {
            continue; // (a corpse is a rigid body, drawn as its cells)
        }
        bool moving = c->walk != 0 && c->grounded;
        bool flash = c->hit_flash > 0;
        if (c->kind == KIND_WALKER) {
            int pose = moving ? (c->x / 3) & 1 : 0;
            draw_sprite(app, k_walker[pose], c->w, c->h, c->x, c->y, c->facing < 0, flash, false,
                        true);
        } else {
            static const int8_t k_flap[4] = {0, 1, 2, 1};
            int wing = k_flap[(app->world->tick / 6) & 3];
            draw_sprite(app, k_flyer[wing], c->w, c->h, c->x, c->y, c->facing < 0, flash, false,
                        true);
        }
    }
    draw_player_sprite(app);
}

static void draw_hud(App *app) {
    const Creature *c = player(app);
    if (!c) {
        return;
    }
    Ui hud;
    ui_begin(&hud, app->renderer, 0, 0, false);
    float x = app->view.x + 16.0f, y = app->view.y + app->view.h - 44.0f;
    float bw = 220.0f, bh = 14.0f;
    float f = PR_CLAMP(c->hp / c->hp_max, 0.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(app->renderer, 30, 8, 8, 255);
    SDL_FRect back = {x - 2, y - 2, bw + 4, bh + 4};
    SDL_RenderFillRect(app->renderer, &back);
    SDL_SetRenderDrawColor(app->renderer, 200, 30, 30, 255);
    SDL_FRect bar = {x, y, bw * f, bh};
    SDL_RenderFillRect(app->renderer, &bar);
    ui_text(&hud, x + 6, y + 3, 0xFFFFFF, "HP %d / %d", (int)ceilf(c->hp), (int)c->hp_max);
    if (c->thermal != 0) {
        ui_text(&hud, x + bw + 12, y + 3, c->thermal > 0 ? 0xFF6030 : 0x60A8FF, "%s",
                c->thermal > 0 ? "BURNING" : "FREEZING");
    }
    const Rifle *r = &app->rifle;
    if (r->reload > 0) {
        ui_text(&hud, x, y - 14, 0xFFC060, "RELOADING");
    }
    ui_text(&hud, x + bw + 16, y + 3, 0xE8E8E8, "SLOT %d / %d", PR_MIN(r->next_slot + 1, (int)r->nslots),
            (int)r->nslots);
    if (pr_time_seconds() < app->toast_until) {
        ui_text(&hud, app->view.x + 12.0f, app->view.y + 12.0f, 0xFFE070, "%s", app->toast);
    }
    if (c->state == CREATURE_DEAD) {
        ui_text(&hud, app->view.x + app->view.w * 0.5f - 80, app->view.y + app->view.h * 0.4f,
                0xFF6040, "YOU DIED   F5 restarts");
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

    bool in_menu = gunmenu_open(&app->menu); // the game waits while a menu is open
    if (app->cfg.frames > 0) {
        if (!app->paused && !in_menu) {
            run_tick(app); // deterministic: exactly one tick per frame
        }
    } else if (app->paused || in_menu) {
        app->accumulator = 0.0;
        if (app->step_once) {
            run_tick(app);
        }
    } else {
        app->accumulator += frame_dt;
        int ticks = 0;
        // A tick slower than real time cannot be caught up: run one per frame (the game slows
        // down a little) instead of several (the frame rate would collapse).
        int max_ticks = MAX_TICKS_PER_FRAME;
        while (app->accumulator >= TICK_DT && ticks < max_ticks) {
            double t0 = pr_time_seconds();
            run_tick(app);
            if (pr_time_seconds() - t0 > TICK_DT) {
                max_ticks = 1;
            }
            app->accumulator -= TICK_DT;
            ticks++;
        }
        if (ticks == max_ticks && app->accumulator >= TICK_DT) {
            app->accumulator = 0.0; // too slow: drop the backlog instead of spiralling
        }
    }
    app->step_once = false;
    {
        // Sound: what the simulation queued, heard from the player (or the screen's centre).
        const Creature *pc = player(app);
        float lx = app->cam_x + app->view.w / zoom(app) * 0.5f;
        float ly = app->cam_y + app->view.h / zoom(app) * 0.5f;
        float jet = 0.0f;
        if (pc) {
            lx = (float)pc->x + (float)pc->w * 0.5f;
            ly = (float)pc->y + (float)pc->h * 0.5f;
            jet = pc->state != CREATURE_DEAD && pc->jet_hold >= JETPACK_DELAY && !pc->grounded &&
                          !app->paused && !gunmenu_open(&app->menu)
                      ? 1.0f
                      : 0.0f;
        }
        audio_frame(&app->audio, app->world, lx, ly, jet);
    }

    SDL_SetRenderDrawColor(app->renderer, 8, 8, 10, 255);
    SDL_RenderClear(app->renderer);
    SDL_Rect clip = {(int)app->view.x, (int)app->view.y, (int)app->view.w, (int)app->view.h};
    SDL_SetRenderClipRect(app->renderer, &clip);
    if (app->wr.pressure_view || app->wr.temperature_view) {
        render_invalidate(app->world); // these can change anywhere: redraw everything
    }
    camera_follow(app);
    draw_world(app);
    draw_actors(app);
    draw_rifle_and_tracers(app);
    draw_hud(app);
    draw_brush_outline(app);
    SDL_SetRenderClipRect(app->renderer, NULL);
    draw_panel(app);
    if (gunmenu_open(&app->menu)) {
        int ww = 0, wh = 0;
        SDL_GetCurrentRenderOutputSize(app->renderer, &ww, &wh);
        ui_begin(&app->ui, app->renderer, app->mouse_x, app->mouse_y, false);
        gunmenu_draw(&app->menu, &app->ui, ww, wh, &app->rifle);
    }
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
