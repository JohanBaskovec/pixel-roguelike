#pragma once

#include "render/render.h"
#include "sim/edit.h"
#include "sim/sim.h"
#include "game/gunmenu.h"
#include "sim/weapon.h"
#include "ui/ui.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

typedef struct AppConfig {
    int world_w, world_h;
    uint64_t seed;
    int frames;             // > 0: quit after this many frames, running exactly 1 tick per frame
    const char *screenshot; // with frames: save the last frame as BMP here
    bool demo;              // start with the demo scene instead of the level
    int threads;            // simulation threads; 0 = one per logical core
    int menu;               // 1: start with the gun menu open, 2: with the editor (screenshots)
    bool new_game;          // do not load the save (the game still saves when it ends)
    bool no_save;           // neither load nor save
    bool example_bullet;    // slot 2 holds the example bullet: hydrogen, oxygen, heat (screenshots)
} AppConfig;

typedef struct App {
    AppConfig cfg;
    SDL_Window *window;
    SDL_Renderer *renderer;
    World *world;
    JobPool *jobs;
    WorldRenderer wr;
    Ui ui;
    EditQueue edits;

    int tool; // index into the toolbar
    int brush_radius;
    bool brush_square;
    float gas_atm; // pressure the gas and air brushes paint at, atm (- and =)
    bool paused;
    bool step_once;
    bool running;

    float mouse_x, mouse_y;
    bool ui_click;    // left click landed on the panel this frame
    int paint_button; // 0 = not painting, else SDL_BUTTON_LEFT / SDL_BUTTON_RIGHT
    int last_wx, last_wy;
    bool stroke_moved; // a stroke segment was queued by an event this frame

    SDL_FRect panel;
    SDL_FRect view; // the window area showing the world (right of the panel)

    // Camera: world cell at the view's top-left corner, and zoom (screen pixels per cell).
    float cam_x, cam_y;
    int zoom_index;
    bool panning; // middle mouse button held
    bool follow;  // the camera follows the player

    // The player (M6a): its creature index (-1: none), rifle, and trigger.
    int player;
    Rifle rifle;
    bool trigger; // left button held over the world with the rifle tool
    GunMenu menu; // the gun menu (Tab) and the bullet editor

    // The save (D-048): one file, loaded when the game starts, written when it ends (and by F6).
    bool saves;       // enabled: interactive runs only
    char toast[64];   // a message ("Saved") for a moment
    double toast_until;

    // Timing and stats
    double accumulator;
    uint64_t frame_count;
    double stat_t0;
    int stat_frames, stat_ticks;
    double stat_sim_s, stat_upload_s, stat_work_s;
    double fps, sim_ms, upload_ms, work_ms;
    SimStats last_stats;
} App;

bool app_init(App *app, const AppConfig *cfg);
void app_shutdown(App *app);
void app_handle_event(App *app, const SDL_Event *ev);

// Runs one frame: input -> edits -> fixed ticks -> render. Returns false when the app should quit.
bool app_frame(App *app, double frame_dt);
