// Headless simulation benchmark. See docs/PERFORMANCE.md -> Measuring.
//
//   pr-bench [--width N] [--height N] [--ticks N] [--warmup N] [--seed N]

#include "core/common.h"
#include "sim/scene.h"
#include "sim/sim.h"

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

int main(int argc, char **argv) {
    int width = 1024, height = 768, ticks = 600, warmup = 60;
    uint64_t seed = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--width") == 0) {
            width = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--height") == 0) {
            height = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--ticks") == 0) {
            ticks = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--warmup") == 0) {
            warmup = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--seed") == 0) {
            seed = strtoull(argv[i + 1], NULL, 10);
        }
    }
    if (ticks <= 0) {
        ticks = 1;
    }

    World *w = world_create(width, height, seed);
    if (!w) {
        fprintf(stderr, "failed to create world\n");
        return 1;
    }
    scene_bench(w);
    sim_run_ticks(w, warmup);

    double *ms = malloc(sizeof(double) * (size_t)ticks);
    uint64_t visited = 0, moved = 0;
    double total = 0.0;
    for (int t = 0; t < ticks; t++) {
        SimStats s;
        double t0 = pr_time_seconds();
        sim_tick(w, &s);
        double dt = pr_time_seconds() - t0;
        ms[t] = dt * 1000.0;
        total += dt;
        visited += s.cells_visited;
        moved += s.cells_moved;
    }
    qsort(ms, (size_t)ticks, sizeof(double), cmp_double);

    printf("pr-bench\n");
    printf("  build      %s\n", BUILD_TYPE);
    printf("  compiler   %s\n", COMPILER);
    printf("  world      %dx%d (%.2f M cells)\n", w->width, w->height,
           (double)w->width * w->height / 1e6);
    printf("  threads    1\n");
    printf("  ticks      %d (after %d warm-up)\n", ticks, warmup);
    printf("  updated    %.0f cells/tick (moved %.0f)\n", (double)visited / ticks,
           (double)moved / ticks);
    printf("  ns/cell    %.2f\n", visited ? total * 1e9 / (double)visited : 0.0);
    printf("  ms/tick    p50 %.3f  p99 %.3f  max %.3f  mean %.3f\n", ms[ticks / 2],
           ms[(int)(ticks * 0.99)], ms[ticks - 1], total * 1000.0 / ticks);

    free(ms);
    world_destroy(w);
    return 0;
}
