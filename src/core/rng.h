#pragma once

#include "core/common.h"

// Hash / seed mixer. Use it to derive seeds, never as a per-cell generator.
static inline uint64_t pr_splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Small, fast generator (xorshift32). One instance per chunk job; never shared between threads.
typedef struct Rng {
    uint32_t s;
} Rng;

static inline Rng rng_from_seed(uint64_t seed) {
    uint32_t s = (uint32_t)(pr_splitmix64(seed) >> 32);
    Rng r = {s ? s : 0x9E3779B9u};
    return r;
}

static inline uint32_t rng_next(Rng *r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

// Uniform integer in [0, n).
static inline uint32_t rng_range(Rng *r, uint32_t n) {
    return (uint32_t)(((uint64_t)rng_next(r) * n) >> 32);
}

static inline bool rng_bit(Rng *r) {
    return (rng_next(r) >> 31) != 0;
}

// True with probability p / 65536 (p in 0..65536).
static inline bool rng_chance16(Rng *r, uint32_t p) {
    return (rng_next(r) >> 16) < p;
}

// Uniform float in [0, 1).
static inline float rng_unit(Rng *r) {
    return (float)(rng_next(r) >> 8) * (1.0f / 16777216.0f);
}
