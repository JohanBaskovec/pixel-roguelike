#include "sim/save.h"

#include "core/rng.h"
#include "sim/bullet.h"
#include "sim/particle.h"
#include "sim/pickup.h"
#include "sim/rigid.h"

#include <string.h>

#define SAVE_MAGIC "PRSAVE01"

// The sizes of what is dumped as it is in memory: a save from a build where one differs is refused.
typedef struct SaveHeader {
    char magic[8];
    uint32_t sizes[8];
    int32_t width, height;
    uint64_t seed, tick;
    int32_t particles, creatures, pickups, rigids;
    uint32_t creature_next_id;
    int32_t player;
} SaveHeader;

static void fill_sizes(uint32_t *s) {
    s[0] = (uint32_t)sizeof(Particle);
    s[1] = (uint32_t)sizeof(Creature);
    s[2] = (uint32_t)sizeof(Pickup);
    s[3] = (uint32_t)sizeof(Rigid);
    s[4] = (uint32_t)sizeof(Rifle);
    s[5] = (uint32_t)sizeof(GasMix);
    s[6] = ELEM_COUNT;
    s[7] = 0;
}

// ---- Run-length streams of fixed-size records: (run: u32, record) pairs ----

typedef struct RleOut {
    FILE *f;
    uint8_t rec[32];
    size_t size;
    uint32_t run;
    bool ok;
} RleOut;

static void rle_flush(RleOut *o) {
    if (o->run > 0) {
        o->ok = o->ok && fwrite(&o->run, sizeof(o->run), 1, o->f) == 1 &&
                fwrite(o->rec, o->size, 1, o->f) == 1;
        o->run = 0;
    }
}

static void rle_push(RleOut *o, const void *rec) {
    if (o->run > 0 && o->run < UINT32_MAX && memcmp(o->rec, rec, o->size) == 0) {
        o->run++;
        return;
    }
    rle_flush(o);
    memcpy(o->rec, rec, o->size);
    o->run = 1;
}

typedef struct RleIn {
    FILE *f;
    uint8_t rec[32];
    size_t size;
    uint32_t run;
    bool ok;
} RleIn;

static const uint8_t *rle_next(RleIn *in) {
    if (in->run == 0) {
        in->ok = in->ok && fread(&in->run, sizeof(in->run), 1, in->f) == 1 &&
                 fread(in->rec, in->size, 1, in->f) == 1 && in->run > 0;
        if (!in->ok) {
            return NULL;
        }
    }
    in->run--;
    return in->rec;
}

// ---- Saving ----

bool world_save(const World *w, FILE *f, const SaveExtra *extra) {
    SaveHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, SAVE_MAGIC, 8);
    fill_sizes(h.sizes);
    h.width = w->width;
    h.height = w->height;
    h.seed = w->seed;
    h.tick = w->tick;
    h.particles = w->particle_count;
    h.creatures = w->creature_count;
    h.pickups = w->pickup_count;
    int active = 0;
    for (int i = 0; i < w->rigid_count; i++) {
        active += w->rigids[i].active;
    }
    h.rigids = active;
    h.creature_next_id = w->creature_next_id;
    h.player = extra->player;
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    size_t n = (size_t)w->width * (size_t)w->height;

    // Element and aux of every cell; temperature of every cell; gas of the gas cells.
    RleOut ea = {f, {0}, 2, 0, ok};
    RleOut tp = {f, {0}, sizeof(float), 0, true};
    for (size_t i = 0; i < n; i++) {
        // (A solid's aux is a random bit nothing reads, except as the flow direction of the liquid
        // it melts into; it is not saved, which keeps the runs long.)
        uint8_t aux = g_elem_behavior[w->cells[i].element] <= BEHAVIOR_STATIC ? 0 : w->cells[i].aux;
        uint8_t rec[2] = {w->cells[i].element, aux};
        rle_push(&ea, rec);
    }
    rle_flush(&ea);
    for (size_t i = 0; i < n; i++) {
        rle_push(&tp, &w->temp[i]);
    }
    rle_flush(&tp);
    RleOut gs = {f, {0}, sizeof(GasMix), 0, true};
    for (size_t i = 0; i < n; i++) {
        if (w->cells[i].element == ELEM_GAS) {
            rle_push(&gs, &w->gas[i]);
        }
    }
    rle_flush(&gs);
    ok = ea.ok && tp.ok && gs.ok;
    // The shade of the cells (random: not worth encoding).
    for (size_t i = 0; i < n && ok; i += 4096) {
        uint8_t buf[4096];
        size_t m = PR_MIN((size_t)4096, n - i);
        for (size_t k = 0; k < m; k++) {
            buf[k] = w->cells[i + k].shade;
        }
        ok = fwrite(buf, 1, m, f) == m;
    }
    ok = ok && fwrite(w->particles, sizeof(Particle), (size_t)w->particle_count, f) ==
                   (size_t)w->particle_count;
    ok = ok && fwrite(w->creatures, sizeof(Creature), (size_t)w->creature_count, f) ==
                   (size_t)w->creature_count;
    ok = ok && fwrite(w->pickups, sizeof(Pickup), (size_t)w->pickup_count, f) ==
                   (size_t)w->pickup_count;
    for (int i = 0; i < w->rigid_count && ok; i++) {
        if (w->rigids[i].active) {
            ok = fwrite(&w->rigids[i], sizeof(Rigid), 1, f) == 1;
        }
    }
    ok = ok && fwrite(&extra->rifle, sizeof(Rifle), 1, f) == 1;
    return ok;
}

// ---- Loading ----

bool world_load(World *w, FILE *f, SaveExtra *extra) {
    SaveHeader h;
    uint32_t sizes[8];
    fill_sizes(sizes);
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && memcmp(h.magic, SAVE_MAGIC, 8) == 0 &&
              memcmp(h.sizes, sizes, sizeof(sizes)) == 0 && h.width == w->width &&
              h.height == w->height && h.particles >= 0 && h.particles <= PARTICLE_MAX &&
              h.creatures >= 0 && h.creatures <= CREATURE_MAX && h.pickups >= 0 &&
              h.pickups <= PICKUP_MAX && h.rigids >= 0 && h.rigids <= RIGID_MAX;
    world_clear(w);
    if (!ok) {
        return false;
    }
    w->seed = h.seed;
    w->tick = h.tick;
    size_t n = (size_t)w->width * (size_t)w->height;
    uint8_t stamp = (uint8_t)(w->tick - 1); // simulated on the next tick
    RleIn ea = {f, {0}, 2, 0, true};
    for (size_t i = 0; i < n; i++) {
        const uint8_t *rec = rle_next(&ea);
        if (!rec || (rec[0] >= ELEM_COUNT && rec[0] != ELEM_VOID)) {
            world_clear(w);
            return false;
        }
        w->cells[i] = (Cell){rec[0], stamp, 0, rec[1]};
    }
    RleIn tp = {f, {0}, sizeof(float), 0, true};
    for (size_t i = 0; i < n; i++) {
        const uint8_t *rec = rle_next(&tp);
        if (!rec) {
            world_clear(w);
            return false;
        }
        memcpy(&w->temp[i], rec, sizeof(float));
    }
    RleIn gs = {f, {0}, sizeof(GasMix), 0, true};
    for (size_t i = 0; i < n; i++) {
        if (w->cells[i].element == ELEM_GAS) {
            const uint8_t *rec = rle_next(&gs);
            if (!rec) {
                world_clear(w);
                return false;
            }
            memcpy(&w->gas[i], rec, sizeof(GasMix));
        }
    }
    for (size_t i = 0; i < n && ok; i += 4096) {
        uint8_t buf[4096];
        size_t m = PR_MIN((size_t)4096, n - i);
        ok = fread(buf, 1, m, f) == m;
        for (size_t k = 0; k < m && ok; k++) {
            w->cells[i + k].shade = buf[k];
        }
    }
    ok = ok && fread(w->particles, sizeof(Particle), (size_t)h.particles, f) == (size_t)h.particles;
    ok = ok && fread(w->creatures, sizeof(Creature), (size_t)h.creatures, f) == (size_t)h.creatures;
    ok = ok && fread(w->pickups, sizeof(Pickup), (size_t)h.pickups, f) == (size_t)h.pickups;
    for (int i = 0; i < h.rigids && ok; i++) {
        ok = fread(&w->rigids[i], sizeof(Rigid), 1, f) == 1;
    }
    ok = ok && fread(&extra->rifle, sizeof(Rifle), 1, f) == 1;
    if (!ok) {
        world_clear(w);
        return false;
    }
    w->particle_count = h.particles;
    w->creature_count = h.creatures;
    w->pickup_count = h.pickups;
    w->rigid_count = h.rigids;
    w->creature_next_id = h.creature_next_id;
    extra->player = h.player;
    // The payloads of bullets in flight are not saved: they fly on as plain bullets.
    for (int i = 0; i < w->particle_count; i++) {
        if (w->particles[i].cell.element == ELEM_BULLET) {
            w->particles[i].cell.aux = 0;
        }
    }
    // Everything is awake (water bodies, heat and activity are worked out again).
    world_wake_rect(w, 0, 0, w->width - 1, w->height - 1);
    world_wake_heat(w, 0, 0, w->width - 1, w->height - 1);
    return true;
}
