#include "core/common.h"
#include "core/log.h"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <stdlib.h>
#include <string.h>

void pr_assert_fail(const char *expr, const char *file, int line) {
    pr_log_error("Assertion failed: %s (%s:%d)", expr, file, line);
    abort();
}

void *pr_aligned_alloc_zero(size_t alignment, size_t size) {
    void *p = SDL_aligned_alloc(alignment, size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void pr_aligned_free(void *ptr) {
    SDL_aligned_free(ptr);
}

double pr_time_seconds(void) {
    static double inv_freq = 0.0;
    if (inv_freq == 0.0) {
        inv_freq = 1.0 / (double)SDL_GetPerformanceFrequency();
    }
    return (double)SDL_GetPerformanceCounter() * inv_freq;
}
