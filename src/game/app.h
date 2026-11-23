#pragma once

#include "render/render.h"
#include "sim/edit.h"
#include "sim/sim.h"
#include "ui/ui.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

typedef struct AppConfig {
    int world_w, world_h;
    uint64_t seed;
    int frames;             // > 0: quit after this many frames, running exactly 1 tick per frame
    const char *screenshot; // with frames: save the last frame as BMP here
    bool demo;              // start with the demo scene instead of the empty box
    int threads;            // simulation threads; 0 = one per logical core
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
