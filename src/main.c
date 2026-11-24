#include "core/log.h"
#include "game/app.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdlib.h>
#include <string.h>

static void usage(void) {
    pr_log_info("Usage: pixel-roguelike [--width N] [--height N] [--seed N] [--demo]\n"
                "                       [--threads N] [--frames N] [--screenshot file.bmp]\n"
                "                       [--menu gun|editor] [--example-bullet]");
}

int main(int argc, char *argv[]) {
    AppConfig cfg = {0};
    cfg.world_w = 4096;
    cfg.world_h = 2048;
    cfg.seed = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_value = i + 1 < argc;
        if (strcmp(a, "--width") == 0 && has_value) {
            cfg.world_w = atoi(argv[++i]);
        } else if (strcmp(a, "--height") == 0 && has_value) {
            cfg.world_h = atoi(argv[++i]);
        } else if (strcmp(a, "--seed") == 0 && has_value) {
            cfg.seed = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(a, "--frames") == 0 && has_value) {
            cfg.frames = atoi(argv[++i]);
        } else if (strcmp(a, "--screenshot") == 0 && has_value) {
            cfg.screenshot = argv[++i];
        } else if (strcmp(a, "--threads") == 0 && has_value) {
            cfg.threads = atoi(argv[++i]);
        } else if (strcmp(a, "--menu") == 0 && has_value) {
            cfg.menu = strcmp(argv[++i], "editor") == 0 ? 2 : 1;
        } else if (strcmp(a, "--example-bullet") == 0) {
            cfg.example_bullet = true;
        } else if (strcmp(a, "--demo") == 0) {
            cfg.demo = true;
        } else {
            usage();
            return 1;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        pr_log_error("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    static App app; // large (edit queue); keep it off the stack
    int status = 0;
    if (app_init(&app, &cfg)) {
        double prev = pr_time_seconds();
        bool running = true;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                app_handle_event(&app, &ev);
            }
            double now = pr_time_seconds();
            running = app_frame(&app, now - prev);
            prev = now;
        }
    } else {
        status = 1;
    }
    app_shutdown(&app);
    SDL_Quit();
    return status;
}
