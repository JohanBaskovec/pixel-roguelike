#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PR_UNUSED(x)        ((void)(x))
#define PR_ARRAY_COUNT(a)   (sizeof(a) / sizeof((a)[0]))
#define PR_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define PR_MAX(a, b)        ((a) > (b) ? (a) : (b))
#define PR_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

#if defined(__GNUC__) || defined(__clang__)
#define PR_LIKELY(x)   __builtin_expect(!!(x), 1)
#define PR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define PR_LIKELY(x)   (x)
#define PR_UNLIKELY(x) (x)
#endif

void pr_assert_fail(const char *expr, const char *file, int line);

#define PR_ASSERT_ALWAYS(cond)                                                                     \
    do {                                                                                           \
        if (PR_UNLIKELY(!(cond))) {                                                                \
            pr_assert_fail(#cond, __FILE__, __LINE__);                                             \
        }                                                                                          \
    } while (0)

#ifdef NDEBUG
#define PR_ASSERT(cond) ((void)0)
#else
#define PR_ASSERT(cond) PR_ASSERT_ALWAYS(cond)
#endif

// Large, long-lived buffers. Aligned to 64 bytes (a cache line). Contents are zeroed.
void *pr_aligned_alloc_zero(size_t alignment, size_t size);
void pr_aligned_free(void *ptr);

// Monotonic high-resolution clock, in seconds.
double pr_time_seconds(void);
