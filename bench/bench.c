// Headless simulation benchmark. See docs/PERFORMANCE.md -> Measuring.
//
//   pr-bench [--scene bench|lava|fire] [--width N] [--height N] [--ticks N] [--warmup N] [--seed N]
//            [--threads N] [--fill F] [--temp-spread K]
//
// --scene:       bench (default): the churning scene below. lava: the default level (4096 x 2048,
//                simulated around the player as in the game, D-040) with a film of lava 300 cells
//                long and 2 deep laid on the floor beside the player; the warm-up (default 300
//                ticks, 5 s) lets the air over it start to convect (D-051, D-052).
//                fire: the same level; the player fires the plain rifle without stopping, sweeping
//                the aim from side to side over the upper half circle every 4 s (warm-up 300).
// --threads:     total threads (0 = one per logical core, the default; 1 = single-threaded).
// --fill:        fraction (0..1) of the space above the rock floor filled with churning matter.
// --temp-spread: give every cell a random temperature within +-K of 20 degC (default 0: all at
//                20 degC, so the heat pass sleeps). Exercises conduction everywhere.

#include "core/common.h"
#include "core/jobs.h"
#include "core/rng.h"
#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/scene.h"
#include "sim/sim.h"
#include "sim/weapon.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#if defined(NDEBUG)
#define BUILD_TYPE "optimised (NDEBUG)"
#else
#define BUILD_TYPE "DEBUG - do not quote these numbers"
#endif

#if defined(__clang__)
#define COMPILER "clang " __clang_version__
#elif defined(__GNUC__)
#define COMPILER "gcc " __VERSION__
#elif defined(_MSC_VER)
#define COMPILER "msvc"
#else
#define COMPILER "unknown"
#endif

// One tick of the level scenes: the area around the player, and the rifle in the fire scene.
static void level_tick(World *w, int pl, bool fire, Rifle *rifle, SimStats *s) {
    Creature *p = &w->creatures[pl];
    world_simulate_near(w, p->x + p->w / 2, p->y + p->h / 2);
    if (fire) {
        p->hp = p->hp_max; // monsters may come: keep firing
        p->health = 1.0f;
        float a = 3.14159265f * (0.5f + 0.5f * cosf((float)w->tick * (2.0f * 3.14159265f / 240.0f)));
        float sx, sy;
        rifle_shoulder(p, &sx, &sy);
        p->facing = cosf(a) >= 0.0f ? 1 : -1;
        rifle_update(w, rifle, pl, sx + 200.0f * cosf(a), sy - 200.0f * sinf(a), true);
    }
    sim_tick(w, s);
}

int main(int argc, char **argv) {
    int width = 1024, height = 768, ticks = 600, warmup = -1, threads = 0;
    bool lava = false, fire = false, sized = false;
    float fill = 1.0f, temp_spread = 0.0f;
    uint64_t seed = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--scene") == 0) {
            lava = strcmp(argv[i + 1], "lava") == 0;
            fire = strcmp(argv[i + 1], "fire") == 0;
            if (!lava && !fire && strcmp(argv[i + 1], "bench") != 0) {
                fprintf(stderr, "unknown scene %s (bench, lava, fire)\n", argv[i + 1]);
                return 1;
            }
        } else if (strcmp(argv[i], "--width") == 0) {
            width = atoi(argv[i + 1]);
            sized = true;
        } else if (strcmp(argv[i], "--height") == 0) {
            height = atoi(argv[i + 1]);
            sized = true;
        } else if (strcmp(argv[i], "--ticks") == 0) {
            ticks = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--warmup") == 0) {
            warmup = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--seed") == 0) {
            seed = strtoull(argv[i + 1], NULL, 10);
        } else if (strcmp(argv[i], "--threads") == 0) {
            threads = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--fill") == 0) {
            fill = (float)atof(argv[i + 1]);
        } else if (strcmp(argv[i], "--temp-spread") == 0) {
            temp_spread = (float)atof(argv[i + 1]);
        }
    }
    if (ticks <= 0) {
        ticks = 1;
    }
    const bool level = lava || fire;
    if (level && !sized) {
        width = 4096;
        height = 2048;
    }
    if (warmup < 0) {
        warmup = level ? 300 : 60;
    }

    World *w = world_create(width, height, seed);
    if (!w) {
        fprintf(stderr, "failed to create world\n");
        return 1;
    }
    JobPool *pool = jobs_create(threads);
    world_set_job_pool(w, pool);
    int pl = -1;
    Rifle rifle = {0};
    if (level) {
        scene_level(w);
        int sx, sy;
        scene_level_start(w, &sx, &sy);
        pl = scene_spawn_player_from(w, sx, sy);
        if (pl < 0) {
            fprintf(stderr, "no room for the player\n");
            return 1;
        }
        for (int t = 0; t < 120; t++) { // the level settles
            level_tick(w, pl, false, &rifle, NULL);
        }
        const Creature *player = &w->creatures[pl];
        for (int k = 0; k < 2 && lava; k++) {
            int y = player->y + player->h - 1 - k; // on the floor the player stands on
            Edit film = {player->x + 20, y, player->x + 320, y, 0, EDIT_SQUARE, ELEM_LAVA, 0,
                         EDIT_PAINT, 0.0f, 0.0f};
            sim_apply_edit(w, &film);
        }
    } else {
        scene_bench(w, fill);
    }
    if (temp_spread > 0.0f) {
        Rng rng = rng_from_seed(seed ^ 0x7E3Full);
        for (int i = 0; i < w->width * w->height; i++) {
            float r = (float)rng_next(&rng) / 4294967296.0f;
            w->temp[i] = 293.15f + (r * 2.0f - 1.0f) * temp_spread;
        }
        world_wake_heat(w, 0, 0, w->width - 1, w->height - 1);
    }
    for (int t = 0; t < warmup; t++) {
        if (level) {
            level_tick(w, pl, fire, &rifle, NULL);
        } else {
            sim_tick(w, NULL);
        }
    }

    double *ms = malloc(sizeof(double) * (size_t)ticks);
    uint64_t visited = 0, moved = 0, heat_chunks = 0, gas_transfers = 0, particles = 0;
    double total = 0.0;
    for (int t = 0; t < ticks; t++) {
        SimStats s;
        double t0 = pr_time_seconds();
        if (level) {
            level_tick(w, pl, fire, &rifle, &s);
        } else {
            sim_tick(w, &s);
        }
        double dt = pr_time_seconds() - t0;
        ms[t] = dt * 1000.0;
        total += dt;
        visited += s.cells_visited;
        moved += s.cells_moved;
        heat_chunks += s.heat_chunks;
        gas_transfers += s.gas_transfers;
        particles += s.particles;
    }
    qsort(ms, (size_t)ticks, sizeof(double), cmp_double);

    printf("pr-bench\n");
    printf("  build      %s\n", BUILD_TYPE);
    printf("  compiler   %s\n", COMPILER);
    printf("  scene      %s\n", lava   ? "lava (the level, a lava film beside the player)"
                                : fire ? "fire (the level, the player fires without stopping)"
                                       : "bench");
    if (level) {
        printf("  world      %dx%d (%.2f M cells; simulated around the player)\n", w->width,
               w->height, (double)w->width * w->height / 1e6);
    } else {
        printf("  world      %dx%d (%.2f M cells), fill %.2f, temp spread %.0f K\n", w->width,
               w->height, (double)w->width * w->height / 1e6, (double)fill, (double)temp_spread);
    }
    printf("  threads    %d\n", jobs_thread_count(pool));
    printf("  ticks      %d (after %d warm-up)\n", ticks, warmup);
    printf("  updated    %.0f cells/tick (moved %.0f)\n", (double)visited / ticks,
           (double)moved / ticks);
    printf("  heat       %.0f chunks/tick\n", (double)heat_chunks / ticks);
    printf("  gas        %.0f transfers/tick\n", (double)gas_transfers / ticks);
    printf("  particles  %.0f in flight\n", (double)particles / ticks);
    printf("  ns/cell    %.2f (wall time / updated cells; lower with more threads)\n",
           visited ? total * 1e9 / (double)visited : 0.0);
    printf("  ms/tick    p50 %.3f  p99 %.3f  max %.3f  mean %.3f\n", ms[ticks / 2],
           ms[(int)(ticks * 0.99)], ms[ticks - 1], total * 1000.0 / ticks);

    free(ms);
    world_destroy(w);
    jobs_destroy(pool);
    return 0;
}
