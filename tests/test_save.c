#include "test.h"

#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/enemy.h"
#include "sim/particle.h"
#include "sim/pickup.h"
#include "sim/rigid.h"
#include "sim/save.h"
#include "sim/scene.h"
#include "sim/sim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void test_save_and_load_round_trip(void);
void test_save_refuses_other_sizes_and_cut_files(void);

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT, 0.0f, 0.0f};
        sim_apply_edit(w, &ed);
    }
}

// A world with a bit of everything, run for a while.
static World *busy_world(SaveExtra *extra) {
    World *w = test_world_box(128, 128, 180);
    fill(w, 1, 1, 126, 126, EDIT_AIR);
    fill(w, 10, 100, 40, 126, ELEM_WATER);
    fill(w, 60, 60, 70, 70, ELEM_HYDROGEN);
    fill(w, 90, 100, 110, 126, ELEM_LAVA);
    fill(w, 80, 20, 85, 30, ELEM_SAND);
    int pl = scene_spawn_player(w, 50);
    enemy_spawn(w, KIND_WALKER, 100, 20);
    rigid_spawn_box(w, 30.0f, 40.0f, 24, 4, 0.0f);
    pickup_add(w, PICKUP_HEALTH, 60, 126);
    Cell bullet = {ELEM_BULLET, 0, 0, 0};
    Particle p = {20.5f, 10.5f, 0.3f, 0.0f, bullet, T_ROOM};
    particle_add(w, &p);
    sim_run_ticks(w, 40);
    memset(extra, 0, sizeof(*extra));
    rifle_init(&extra->rifle);
    extra->rifle.slots[1].px[0] =
        (BulletPixel){BULLET_MATTER, ELEM_HYDROGEN, RELEASE_GRADUAL, 300.0f, 2.5f, 0.0f};
    extra->rifle.fire_interval = 7;
    extra->player = pl;
    return w;
}

static bool same_state(const World *a, const World *b) {
    size_t n = (size_t)a->width * (size_t)a->height;
    for (size_t i = 0; i < n; i++) {
        bool inert = g_elem_behavior[a->cells[i].element] <= BEHAVIOR_STATIC; // aux is not saved
        if (a->cells[i].element != b->cells[i].element ||
            (!inert && a->cells[i].aux != b->cells[i].aux) ||
            a->cells[i].shade != b->cells[i].shade) {
            return false;
        }
    }
    return memcmp(a->temp, b->temp, n * sizeof(float)) == 0 &&
           memcmp(a->gas, b->gas, n * sizeof(GasMix)) == 0;
}

void test_save_and_load_round_trip(void) {
    // Everything comes back: the cells (with their gas and heat), the creatures, bodies, crates and
    // particles, the gun with its bullet designs; and the two worlds then go on identically.
    SaveExtra ex, back;
    World *a = busy_world(&ex);
    FILE *f = tmpfile();
    EXPECT(f != NULL);
    EXPECT(world_save(a, f, &ex));
    rewind(f);
    World *b = world_create(a->width, a->height, 999); // another seed: the save brings its own
    memset(&back, 0, sizeof(back));
    EXPECT(world_load(b, f, &back));
    fclose(f);
    EXPECT(same_state(a, b));
    EXPECT_EQ(b->tick, a->tick);
    EXPECT_EQ(b->seed, a->seed);
    EXPECT_EQ(b->particle_count, a->particle_count);
    EXPECT_EQ(b->creature_count, a->creature_count);
    EXPECT_EQ(b->pickup_count, a->pickup_count);
    EXPECT(memcmp(a->creatures, b->creatures, sizeof(Creature) * (size_t)a->creature_count) == 0);
    EXPECT(memcmp(a->particles, b->particles, sizeof(Particle) * (size_t)a->particle_count) == 0);
    EXPECT_EQ(back.player, ex.player);
    EXPECT(memcmp(&back.rifle, &ex.rifle, sizeof(Rifle)) == 0);
    EXPECT_EQ(back.rifle.fire_interval, 7);
    EXPECT(back.rifle.slots[1].px[0].element == ELEM_HYDROGEN);
    int ra = 0, rb = 0;
    for (int i = 0; i < a->rigid_count; i++) {
        ra += a->rigids[i].active;
    }
    for (int i = 0; i < b->rigid_count; i++) {
        rb += b->rigids[i].active;
    }
    EXPECT(ra >= 1 && ra == rb);
    // Going on from the save is physically the same as going on without it, though not cell for
    // cell: the loaded world starts all awake, which changes the order of the random draws (D-048).
    sim_run_ticks(a, 120);
    sim_run_ticks(b, 120);
    for (int e = 0; e < ELEM_COUNT; e++) {
        int64_t ca = test_count(a, (ElementId)e), cb = test_count(b, (ElementId)e);
        EXPECT(llabs(ca - cb) <= 2 + ca / 50); // within 2%
    }
    double ga = 0.0, gb = 0.0;
    for (size_t i = 0; i < (size_t)a->width * (size_t)a->height; i++) {
        ga += (double)gas_total(&a->gas[i]);
        gb += (double)gas_total(&b->gas[i]);
    }
    EXPECT(fabs(ga - gb) <= 1e-3 * ga);
    world_destroy(a);
    world_destroy(b);
}

void test_save_refuses_other_sizes_and_cut_files(void) {
    SaveExtra ex, back;
    World *a = busy_world(&ex);
    FILE *f = tmpfile();
    EXPECT(world_save(a, f, &ex));
    long size = ftell(f);
    // Another size of world: refused, and the world is left empty.
    rewind(f);
    World *big = world_create(192, 128, 1);
    fill(big, 5, 5, 20, 20, ELEM_DIRT);
    EXPECT(!world_load(big, f, &back));
    EXPECT_EQ(test_count(big, ELEM_DIRT), 0);
    world_destroy(big);
    // A file cut short: refused.
    rewind(f);
    char *buf = malloc((size_t)size);
    EXPECT(fread(buf, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    f = tmpfile();
    fwrite(buf, 1, (size_t)size / 2, f);
    rewind(f);
    free(buf);
    World *c = world_create(a->width, a->height, 2);
    EXPECT(!world_load(c, f, &back));
    EXPECT_EQ(test_count(c, ELEM_WATER), 0); // empty
    fclose(f);
    // Not a save at all.
    f = tmpfile();
    fputs("this is not a save of anything, but long enough to hold a header of some kind ......", f);
    rewind(f);
    EXPECT(!world_load(c, f, &back));
    fclose(f);
    world_destroy(c);
    world_destroy(a);
}
