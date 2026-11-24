// Prototype, not part of the game or of the CMake build: air with a velocity
// (docs/PERFORMANCE.md -> Proposed next steps, step 3; docs/DECISIONS.md D-036).
//
// Gas cells as in the game: a mixture (kg of 5 species, one struct per cell) and a temperature.
// Added: a velocity on every face between two cells (u on the face to the right of a cell, v on
// the face below it), in cells per tick. Each substep:
//   1. pressure differences accelerate the face velocities (u -= c^2 dt dP/P0), which are damped
//      and held at 0 on faces next to walls: a one-cell wall, straight or diagonal, is airtight;
//   2. gas crosses every face: velocity x the upwind cell's moles, its mixture and heat; at most
//      FLOW_MAX of a cell per face, so no cell goes negative;
//   3. all crossings are applied at once (computed from the substep's start: no order).
// Mass, each species and energy are conserved exactly (up to float rounding). Sound travels at
// SOUND cells per tick instead of 340 m/s (113 cells per tick): explicit, stable for
// SOUND * dt <= 0.7.
//
// Build and run (POSIX): sound in cells per tick, substeps per tick, damping per substep
//   gcc -O3 -std=c17 bench/proto_air_velocity.c -o proto_air_velocity -lm
//   ./proto_air_velocity 2 4 0.005
//
// Measured (2026-11-24, 4-core VM, one thread; docs/PERFORMANCE.md -> Proposed next steps):
// stable for SOUND / SUBSTEPS <= 0.5 with FLOW_MAX 0.1 (0.2 let a cell lose 80% of its gas in a
// substep through its four faces, and a checkerboard of 3.3 / 0.7 atm locked in; unclamped
// velocities piled gas up to 3.5 atm). Sealed one-cell walls, straight or diagonal: nothing
// crosses in 2000 ticks. A floor patch held at 1000 K for 20 s: the room's pressure stays even
// (1.0025 atm over the heat and far away). Two rooms at 2 and 1 atm through a 12-cell gap: within
// 1% after 8.4 s at sound 2 / 4 substeps (19 s at 1 / 2, 6.3 s at 3 / 6). Cost: 16-17 ns per cell
// per substep.
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NS 5
static const float inv_m[NS] = {31.25f, 22.72f, 496.0f, 55.5f, 35.7f}; // mol/kg: O2 CO2 H2 H2O N2
static const float cp[NS] = {918.0f, 844.0f, 14300.0f, 2080.0f, 1040.0f};

typedef struct GasMix {
    float m[NS];
} GasMix;

static int W, H;
static GasMix *g, *gn;
static float *T, *E, *u, *v, *pr, *nm;
static unsigned char *wall;

static float SOUND = 2.0f; // cells per tick
static int SUBSTEPS = 4;   // per tick
static float DAMP = 0.05f; // velocity lost per substep (sound attenuation)
#define FLOW_MAX 0.1f      // most of a cell crossing one face per substep (4 faces: 40%)
static double P0 = 1.0;    // reference n*T (1 atm, 293 K, one cell)

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static inline size_t at(int x, int y) {
    return (size_t)y * W + x;
}
static inline float moles(const GasMix *a) {
    float n = 0.0f;
    for (int s = 0; s < NS; s++) {
        n += a->m[s] * inv_m[s];
    }
    return n;
}
static inline float heat_cap(const GasMix *a) {
    float c = 0.0f;
    for (int s = 0; s < NS; s++) {
        c += a->m[s] * cp[s];
    }
    return c;
}

// One substep over rows [y0, y1) (the whole grid here; the game would do active tiles).
static void substep(float dt) {
    float k = SOUND * SOUND * dt / (float)P0;
    // A face carries at most FLOW_MAX of a cell per substep, and gas does not flow faster than
    // sound (a gap chokes): the velocity is held within both, so momentum never builds up that the
    // flow cannot carry (it piled gas up and overshot: 3.5 atm from a 2 atm room).
    float vmax = fminf(SOUND, FLOW_MAX / dt);
    // pressure (as n*T) and moles per cell
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t i = at(x, y);
            float n = wall[i] ? 0.0f : moles(&g[i]);
            nm[i] = n;
            pr[i] = n * T[i];
        }
    }
    // 1. velocities
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t i = at(x, y);
            if (x + 1 < W) {
                size_t j = i + 1;
                u[i] =
                    (wall[i] | wall[j])
                        ? 0.0f
                        : fmaxf(-vmax, fminf(vmax, (u[i] - k * (pr[j] - pr[i])) * (1.0f - DAMP)));
            }
            if (y + 1 < H) {
                size_t j = i + (size_t)W;
                v[i] =
                    (wall[i] | wall[j])
                        ? 0.0f
                        : fmaxf(-vmax, fminf(vmax, (v[i] - k * (pr[j] - pr[i])) * (1.0f - DAMP)));
            }
        }
    }
    // 2-3. fluxes, gathered per cell from its 4 faces (each face computed identically by both
    // cells: conservative and order-free)
    for (int y = 1; y < H - 1; y++) {
        for (int x = 1; x < W - 1; x++) {
            size_t i = at(x, y);
            gn[i] = g[i];
            if (wall[i]) {
                continue;
            }
            float e = heat_cap(&g[i]) * T[i];
            // faces: right (i -> i+1, velocity u[i]), left (i-1 -> i, u[i-1]), down (v[i]), up
            const size_t nb[4] = {i + 1, i - 1, i + (size_t)W, i - (size_t)W};
            const float vel[4] = {u[i], -u[i - 1], v[i], -v[i - (size_t)W]}; // outward from i
            for (int f = 0; f < 4; f++) {
                float c = vel[f] * dt; // fraction of the donor crossing
                if (c == 0.0f) {
                    continue;
                }
                size_t donor = c > 0.0f ? i : nb[f];
                float frac = fminf(fabsf(c), FLOW_MAX);
                float sign = c > 0.0f ? -1.0f : 1.0f; // out of i, or into i
                for (int s = 0; s < NS; s++) {
                    gn[i].m[s] += sign * frac * g[donor].m[s];
                }
                e += sign * frac * heat_cap(&g[donor]) * T[donor];
            }
            E[i] = e;
        }
    }
    for (int y = 1; y < H - 1; y++) {
        for (int x = 1; x < W - 1; x++) {
            size_t i = at(x, y);
            if (wall[i]) {
                continue;
            }
            g[i] = gn[i];
            float c = heat_cap(&g[i]);
            T[i] = c > 0.0f ? E[i] / c : T[i];
        }
    }
}

static void tick(void) {
    for (int s = 0; s < SUBSTEPS; s++) {
        substep(1.0f / (float)SUBSTEPS);
    }
}

static GasMix air(float atm) {
    float n = (float)(P0 / 293.15) * atm; // moles for n*T = atm * P0 at 293.15 K
    GasMix a = {{0}};
    a.m[0] = n * 0.21f / inv_m[0];
    a.m[4] = n * 0.79f / inv_m[4];
    return a;
}

static void setup(int w, int h) {
    W = w;
    H = h;
    size_t N = (size_t)W * H;
    free(g);
    free(gn);
    free(T);
    free(E);
    free(u);
    free(v);
    free(pr);
    free(nm);
    free(wall);
    g = calloc(N, sizeof(GasMix));
    gn = calloc(N, sizeof(GasMix));
    T = calloc(N, 4);
    E = calloc(N, 4);
    u = calloc(N, 4);
    v = calloc(N, 4);
    pr = calloc(N, 4);
    nm = calloc(N, 4);
    wall = calloc(N, 1);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t i = at(x, y);
            wall[i] = x == 0 || y == 0 || x == W - 1 || y == H - 1;
            g[i] = air(1.0f);
            T[i] = 293.15f;
        }
    }
}

static double total_moles(int x0, int y0, int x1, int y1, double *energy) {
    double n = 0.0, e = 0.0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            size_t i = at(x, y);
            if (!wall[i]) {
                n += moles(&g[i]);
                e += (double)heat_cap(&g[i]) * T[i];
            }
        }
    }
    if (energy) {
        *energy = e;
    }
    return n;
}

static void p_range(int x0, int y0, int x1, int y1, double *lo, double *hi) {
    *lo = 1e30;
    *hi = 0.0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            size_t i = at(x, y);
            if (!wall[i]) {
                double p = moles(&g[i]) * T[i] / P0;
                *lo = fmin(*lo, p);
                *hi = fmax(*hi, p);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        SOUND = (float)atof(argv[1]);
    }
    if (argc > 2) {
        SUBSTEPS = atoi(argv[2]);
    }
    if (argc > 3) {
        DAMP = (float)atof(argv[3]);
    }
    printf("sound %.2f cells/tick (%.1f m/s), %d substeps, damping %.3f per substep\n", SOUND,
           SOUND * 0.05 * 60, SUBSTEPS, DAMP);

    // A. Two rooms (126 x 62 each) at 2 atm and 1 atm, a one-cell wall with a 12-cell gap at the
    //    bottom: time for the pressures to even out to 1%; mass conserved.
    setup(256, 64);
    for (int y = 1; y < 63; y++) {
        wall[at(128, y)] = y < 51;
        for (int x = 1; x < 128; x++) {
            g[at(x, y)] = air(2.0f);
        }
    }
    double e0, n0 = total_moles(0, 0, W - 1, H - 1, &e0), e1;
    int t_even = -1;
    for (int t = 1; t <= 6000 && t_even < 0; t++) {
        tick();
        double lo, hi;
        p_range(1, 1, W - 2, H - 2, &lo, &hi);
        if (hi < 1.01 * lo) {
            t_even = t;
        }
        if (getenv("TRACE") && t % 60 == 0) {
            printf("   t=%4d spread %.4f (lo %.4f hi %.4f)\n", t, hi - lo, lo, hi);
        }
    }
    if (getenv("TRACE")) {
        int lx = 0, ly = 0, hx = 0, hy = 0;
        double lo = 1e30, hi = 0;
        for (int y = 1; y < H - 1; y++) {
            for (int x = 1; x < W - 1; x++) {
                size_t i = at(x, y);
                if (wall[i]) {
                    continue;
                }
                double p = moles(&g[i]) * T[i] / P0;
                if (p < lo) {
                    lo = p;
                    lx = x;
                    ly = y;
                }
                if (p > hi) {
                    hi = p;
                    hx = x;
                    hy = y;
                }
            }
        }
        printf("   lo %.3f at %d,%d  hi %.3f at %d,%d\n", lo, lx, ly, hi, hx, hy);
        for (int y = hy - 3; y <= hy + 3; y++) {
            printf("   ");
            for (int x = hx - 5; x <= hx + 5; x++) {
                if (x < 0 || y < 0 || x >= W || y >= H) {
                    printf("   .  ");
                    continue;
                }
                size_t i = at(x, y);
                if (wall[i]) {
                    printf("  ##  ");
                } else {
                    printf("%5.2f ", moles(&g[i]) * T[i] / P0);
                }
            }
            printf("\n");
        }
    }
    double n1 = total_moles(0, 0, W - 1, H - 1, &e1);
    printf(
        "A. 2:1 rooms through a 12-cell gap: even within 1%% after %d ticks (%.1f s); moles %+.2e, "
        "energy %+.2e (relative)\n",
        t_even, t_even / 60.0, (n1 - n0) / n0, (e1 - e0) / e0);

    // B. Thin walls: a sealed room (1-cell wall, with a diagonal staircase section) at 2 atm in a
    //    room at 1 atm, for 2000 ticks: what crosses.
    setup(256, 256);
    for (int y = 60; y <= 180; y++) {
        for (int x = 60; x <= 180; x++) {
            bool edge = x == 60 || x == 180 || y == 60 || y == 180;
            if (edge) {
                wall[at(x, y)] = 1;
            } else {
                g[at(x, y)] = air(2.0f);
            }
        }
    }
    // a diagonal staircase cutting the inside in two (cells touch only at corners)
    for (int k = 0; k < 119; k++) {
        wall[at(61 + k, 61 + k)] = 1;
    }
    for (int y = 61; y < 180; y++) {
        for (int x = 61; x < 180; x++) {
            if (!wall[at(x, y)] && x > y) {
                g[at(x, y)] = air(3.0f); // the upper-right half at 3 atm
            }
        }
    }
    double in_a = 0, in_b = 0, out0 = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t i = at(x, y);
            if (wall[i]) {
                continue;
            }
            bool inside = x > 60 && x < 180 && y > 60 && y < 180;
            if (!inside) {
                out0 += moles(&g[i]);
            } else if (x > y) {
                in_a += moles(&g[i]);
            } else {
                in_b += moles(&g[i]);
            }
        }
    }
    for (int t = 0; t < 2000; t++) {
        tick();
    }
    double in_a1 = 0, in_b1 = 0, out1 = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t i = at(x, y);
            if (wall[i]) {
                continue;
            }
            bool inside = x > 60 && x < 180 && y > 60 && y < 180;
            if (!inside) {
                out1 += moles(&g[i]);
            } else if (x > y) {
                in_a1 += moles(&g[i]);
            } else {
                in_b1 += moles(&g[i]);
            }
        }
    }
    printf("B. sealed thin walls (straight and diagonal), 2000 ticks: outside %+.2e, inside 3 atm "
           "half %+.2e, 2 atm half %+.2e (relative change)\n",
           (out1 - out0) / out0, (in_a1 - in_a) / in_a, (in_b1 - in_b) / in_b);

    // C. Steady heating: a 20 x 6 patch on the floor of a 512 x 256 room held at 1000 K (a lava
    //    surface heating the air that touches it), 20 s.
    setup(512, 256);
    for (int t = 0; t < 1200; t++) {
        for (int y = 248; y <= 253; y++) {
            for (int x = 246; x <= 265; x++) {
                T[at(x, y)] = 1000.0f;
            }
        }
        tick();
    }
    double plo, phi, flo, fhi;
    p_range(246, 240, 265, 253, &plo, &phi);
    p_range(10, 10, 60, 60, &flo, &fhi);
    float tmax = 0;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        if (!wall[i] && T[i] > tmax) {
            tmax = T[i];
        }
    }
    printf("C. steady heating (hottest %.0f K): pressure over the heat %.4f-%.4f atm, far away "
           "%.4f-%.4f atm\n",
           tmax, plo, phi, flo, fhi);

    // D. Cost: 1024 x 1024, every cell computed every substep.
    setup(1024, 1024);
    for (int y = 1; y < 1023; y++) {
        for (int x = 1; x < 1023; x++) {
            float r2 = (float)((x - 500) * (x - 500) + (y - 500) * (y - 500));
            if (r2 < 10000) {
                g[at(x, y)] = air(1.5f);
            }
        }
    }
    tick();
    double t0 = now();
    int ticks = 20;
    for (int t = 0; t < ticks; t++) {
        tick();
    }
    double dt = (now() - t0) / ticks;
    printf(
        "D. cost: %.1f ms per tick for 1024 x 1024, %.1f ns per cell per tick (%.1f per substep)\n",
        dt * 1e3, dt * 1e9 / (1024.0 * 1024.0), dt * 1e9 / (1024.0 * 1024.0) / SUBSTEPS);
    return 0;
}
