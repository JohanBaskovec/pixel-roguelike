// Prototype, not part of the game or of the CMake build: the cost of computing gas "as a field"
// (docs/PERFORMANCE.md -> Proposed next steps; docs/DECISIONS.md D-036).
//
// A 1024 x 1024 grid, every cell active: 5 gas species and a temperature per cell, each in its own
// array. One pass per tick: across every face between two cells, a bulk flow driven by the
// pressure difference (carrying the upwind cell's mixture and heat), diffusion of each species
// down its mole-fraction difference, and heat conduction. Each face is computed once and applied
// to both cells, from the previous tick's values (double buffered), so mass and energy are
// conserved and the result does not depend on the order or the thread count. Walls are cells
// with open = 0. No buoyancy, velocity, sleeping or other matter: arithmetic and memory only.
//
// Build and run (POSIX; the pragmas are ignored without -fopenmp, which is used only here):
//   gcc -O3 -std=c17 -fopenmp bench/proto_gas_field.c -o proto_gas_field -lm
//   OMP_NUM_THREADS=4 ./proto_gas_field
// Add -mavx2 -mfma for the AVX2 numbers.
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define W  1024
#define H  1024
#define NS 5
static const float inv_m[NS] = {31.25f, 22.72f, 496.0f, 55.5f, 35.7f};
static const float mol[NS] = {0.032f, 0.044f, 0.002016f, 0.018f, 0.028f};
static const float cp[NS] = {918.0f, 844.0f, 14300.0f, 2080.0f, 1040.0f};
static float *m[NS], *mn[NS], *T, *Tn, *nmol, *inv_n, *pv, *cap, *open_;
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
#define K_FLOW 0.2f
#define K_DIFF 0.0333f
#define K_COND 0.02f

// Coefficients of the faces between row a[0..n) and row b[0..n) (same x, offset by the caller):
// flux of species s from a to b = ca * m_s[a] - cb * m_s[b]; e0: conducted heat.
static void face_coefs(int n, const float *restrict pa, const float *restrict pb,
                       const float *restrict na, const float *restrict nb, const float *restrict ia,
                       const float *restrict ib, const float *restrict oa, const float *restrict ob,
                       const float *restrict ta, const float *restrict tb, const float *restrict ka,
                       const float *restrict kb, float *restrict ca, float *restrict cb,
                       float *restrict e0) {
    for (int x = 0; x < n; x++) {
        float op = oa[x] * ob[x];
        float f = K_FLOW * (pa[x] - pb[x]) / (pa[x] + pb[x] + 1e-20f) * op;
        float fa = f > 0.0f ? f : 0.0f, fb = f < 0.0f ? -f : 0.0f;
        float nbar = 0.5f * (na[x] + nb[x]) * K_DIFF * op;
        ca[x] = fa + nbar * ia[x];
        cb[x] = fb + nbar * ib[x];
        e0[x] = K_COND * op * (ta[x] - tb[x]) * (ka[x] < kb[x] ? ka[x] : kb[x]);
    }
}

// Flux of one species across those faces, and the heat it carries (added to e).
static void face_flux(int n, const float *restrict ma, const float *restrict mb,
                      const float *restrict ca, const float *restrict cb, const float *restrict ta,
                      const float *restrict tb, float c, float *restrict d, float *restrict e) {
    for (int x = 0; x < n; x++) {
        float v = ca[x] * ma[x] - cb[x] * mb[x];
        d[x] = v;
        e[x] += v * c * (v > 0.0f ? ta[x] : tb[x]);
    }
}

#define BAND 64
static void band(int y0, int y1) { // rows y0..y1-1
    float car[W], cbr[W], cad[W], cbd[W];
    float fr[NS][W], fd[2][NS][W], er[W], ed[2][W], e[W], c[W];
    memset(fd, 0, sizeof(fd));
    memset(ed, 0, sizeof(ed));
    for (int y = y0 - 1; y < y1; y++) { // the row above first: only its down faces are needed
        size_t r = (size_t)y * W, rd = r + W;
        int n = W - 1;
        float(*fdc)[W] = fd[y & 1], (*fdp)[W] = fd[(y - 1) & 1];
        float *edc = ed[y & 1], *edp = ed[(y - 1) & 1];
        face_coefs(W, pv + r, pv + rd, nmol + r, nmol + rd, inv_n + r, inv_n + rd, open_ + r,
                   open_ + rd, T + r, T + rd, cap + r, cap + rd, cad, cbd, edc);
        for (int s = 0; s < NS; s++) {
            face_flux(W, m[s] + r, m[s] + rd, cad, cbd, T + r, T + rd, cp[s], fdc[s], edc);
        }
        if (y < y0) {
            continue;
        }
        face_coefs(n, pv + r, pv + r + 1, nmol + r, nmol + r + 1, inv_n + r, inv_n + r + 1,
                   open_ + r, open_ + r + 1, T + r, T + r + 1, cap + r, cap + r + 1, car, cbr, er);
        for (int s = 0; s < NS; s++) {
            face_flux(n, m[s] + r, m[s] + r + 1, car, cbr, T + r, T + r + 1, cp[s], fr[s], er);
        }
        for (int x = 1; x < W - 1; x++) {
            e[x] = cap[r + x] * T[r + x] - er[x] + er[x - 1] - edc[x] + edp[x];
            c[x] = 0.0f;
        }
        for (int s = 0; s < NS; s++) {
            const float *restrict old = m[s] + r, *restrict f = fr[s], *restrict dn = fdc[s],
                                  *restrict up = fdp[s];
            float *restrict out = mn[s] + r;
            float cps = cp[s];
            for (int x = 1; x < W - 1; x++) {
                float v = old[x] - f[x] + f[x - 1] - dn[x] + up[x];
                out[x] = v;
                c[x] += v * cps;
            }
        }
        for (int x = 1; x < W - 1; x++) {
            Tn[r + x] = c[x] > 0.0f ? e[x] / c[x] : T[r + x];
        }
    }
}

static void tick(void) {
#pragma omp parallel for schedule(static)
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t i = (size_t)y * W + x;
            float n = 0.0f, c = 0.0f;
            for (int s = 0; s < NS; s++) {
                n += m[s][i] * inv_m[s];
                c += m[s][i] * cp[s];
            }
            nmol[i] = n;
            inv_n[i] = 1.0f / (n + 1e-20f);
            pv[i] = n * T[i];
            cap[i] = c;
        }
    }
#pragma omp parallel for schedule(dynamic, 1)
    for (int b = 0; b < H / BAND; b++) {
        int y0 = b * BAND, y1 = y0 + BAND;
        band(y0 < 1 ? 1 : y0, y1 > H - 1 ? H - 1 : y1);
    }
    for (int s = 0; s < NS; s++) {
        float *t = m[s];
        m[s] = mn[s];
        mn[s] = t;
    }
    float *t = T;
    T = Tn;
    Tn = t;
}

int main(void) {
    size_t N = (size_t)W * H;
    for (int s = 0; s < NS; s++) {
        m[s] = aligned_alloc(64, N * 4);
        mn[s] = aligned_alloc(64, N * 4);
    }
    T = aligned_alloc(64, N * 4);
    Tn = aligned_alloc(64, N * 4);
    nmol = aligned_alloc(64, N * 4);
    inv_n = aligned_alloc(64, N * 4);
    pv = aligned_alloc(64, N * 4);
    cap = aligned_alloc(64, N * 4);
    open_ = aligned_alloc(64, N * 4);
    unsigned r = 12345;
    for (size_t i = 0; i < N; i++) {
        int x = i % W, y = i / W;
        r = r * 1664525u + 1013904223u;
        float co2 = ((x - 500) * (x - 500) + (y - 400) * (y - 400) < 10000) ? 1.0f : 0.0004f;
        float n = 5.2e-3f * (1.0f + 0.2f * ((r >> 8) & 0xFF) / 255.0f);
        m[0][i] = n * 0.21f * (1 - co2) * mol[0];
        m[1][i] = n * co2 * mol[1];
        m[2][i] = 0.0f;
        m[3][i] = n * 0.01f * mol[3];
        m[4][i] = n * 0.78f * (1 - co2) * mol[4];
        T[i] = 293.0f + (x > 512 ? 30.0f : 0.0f);
        open_[i] =
            (x == 0 || y == 0 || x == W - 1 || y == H - 1 || (x == 300 && y > 200)) ? 0.0f : 1.0f;
        for (int s = 0; s < NS; s++) {
            mn[s][i] = m[s][i];
        }
        Tn[i] = T[i];
    }
    double mass0 = 0, mass1 = 0, e0 = 0, e1 = 0;
    for (size_t i = 0; i < N; i++) {
        if (open_[i] > 0) {
            for (int s = 0; s < NS; s++) {
                mass0 += m[s][i];
                e0 += (double)m[s][i] * cp[s] * T[i];
            }
        }
    }
    tick();
    double t0 = now();
    int ticks = 100;
    for (int k = 0; k < ticks; k++) {
        tick();
    }
    double dt = (now() - t0) / ticks;
    for (size_t i = 0; i < N; i++) {
        if (open_[i] > 0) {
            for (int s = 0; s < NS; s++) {
                mass1 += m[s][i];
                e1 += (double)m[s][i] * cp[s] * T[i];
            }
        }
    }
    float lo = 1e30f, hi = 0;
    for (size_t i = 0; i < N; i++) {
        if (open_[i] > 0) {
            float p = pv[i];
            lo = p < lo ? p : lo;
            hi = p > hi ? p : hi;
        }
    }
    printf("%.2f ms per tick, %.1f ns per cell (wall time), mass drift %.1e, energy drift %.1e, "
           "pressure spread %.3f\n",
           dt * 1e3, dt / N * 1e9, (mass1 - mass0) / mass0, (e1 - e0) / e0, hi / lo);
    return 0;
}
