#include "test.h"

#include "core/jobs.h"
#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/scene.h"
#include "sim/sim.h"
#include "sim/weapon.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void test_rifle_digs_dirt(void);
void test_bullet_stops_at_iron(void);
void test_bullet_stirs_air(void);
void test_shooting_same_for_any_thread_count(void);
void test_small_clumps_fall(void);
void test_player_runs_and_jumps(void);
void test_air_brush_spreads_oxygen(void);
void test_walk_forward_while_shooting(void);
void test_walking_kicks_small_piles(void);
void test_walker_crosses_heaps(void);
void test_falling_powder_does_not_block_walker(void);
void test_player_steers_in_the_air(void);
void test_muzzle_gas(void);
void test_bullets_and_casings_melt(void);

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT};
        sim_apply_edit(w, &ed);
    }
}

// A room of air (192 x 128 world) with a wall of `wall` filling its right part (x >= 160), and
// the player standing on the floor at the left.
static World *range(uint64_t seed, ElementId wall, int *player) {
    World *w = test_world_box(192, 128, seed);
    fill(w, 1, 1, 190, 126, EDIT_AIR);
    fill(w, 160, 1, 190, 126, wall);
    *player = scene_spawn_player(w, 60);
    sim_run_ticks(w, 30);
    return w;
}

// Fires `shots` shots at (dx, dy) from the shoulder, holding the trigger, then lets everything
// settle for `settle` ticks.
static void shoot(World *w, int player, float dx, float dy, int shots, int settle) {
    Rifle r = {0};
    float sx, sy;
    rifle_shoulder(&w->creatures[player], &sx, &sy);
    while ((int)r.shots < shots) {
        rifle_update(w, &r, player, sx + dx, sy + dy, true);
        sim_tick(w, NULL);
    }
    sim_run_ticks(w, settle);
}

void test_rifle_digs_dirt(void) {
    // Ten shots into a dirt wall: a bullet digs up to 30 cm (6 cells) of dirt, which flies out
    // as loose soil; nothing is destroyed (dirt + soil is unchanged). Every bullet and casing ends
    // up as a pixel on the ground, and nothing is left in flight.
    int pl;
    World *w = range(101, ELEM_DIRT, &pl);
    EXPECT(pl >= 0);
    int64_t dirt = test_count(w, ELEM_DIRT);
    shoot(w, pl, 100.0f, 0.0f, 10, 300);
    int64_t soil = test_count(w, ELEM_SOIL);
    EXPECT(soil >= 6); // a full 30 cm; later bullets mostly hit the soil and bullets in the hole
    EXPECT_EQ(test_count(w, ELEM_DIRT) + soil, dirt);
    EXPECT_EQ(test_count(w, ELEM_BULLET), 10);
    EXPECT_EQ(test_count(w, ELEM_CASING), 10);
    EXPECT_EQ(w->particle_count, 0);
    // The casings landed near the shooter, behind the shoulder or below it.
    int64_t near = 0;
    for (int y = 1; y <= 126; y++) {
        for (int x = 1; x < 100; x++) {
            near += test_cell_is(w, x, y, ELEM_CASING);
        }
    }
    EXPECT_EQ(near, 10);
    world_destroy(w);
}

void test_bullet_stops_at_iron(void) {
    // Iron is not dug: the bullets stop at it and drop to the floor in front of it.
    int pl;
    World *w = range(102, ELEM_IRON, &pl);
    int64_t iron = test_count(w, ELEM_IRON);
    shoot(w, pl, 100.0f, 0.0f, 5, 300);
    EXPECT_EQ(test_count(w, ELEM_IRON), iron);
    EXPECT_EQ(test_count(w, ELEM_BULLET), 5);
    int64_t at_wall = 0;
    for (int x = 130; x < 160; x++) { // they bounce back a little as they drop
        for (int y = 110; y <= 126; y++) {
            at_wall += test_cell_is(w, x, y, ELEM_BULLET);
        }
    }
    EXPECT_EQ(at_wall, 5);
    world_destroy(w);
}

void test_bullet_stirs_air(void) {
    // A bullet leaves a current in the air along its path.
    int pl;
    World *w = range(103, ELEM_DIRT, &pl);
    Rifle r = {0};
    float sx, sy;
    rifle_shoulder(&w->creatures[pl], &sx, &sy);
    rifle_update(w, &r, pl, sx + 100.0f, sy, true);
    sim_tick(w, NULL);
    int stirred = 0;
    int row = (int)sy;
    for (int x = (int)sx + 10; x < 150; x++) {
        for (int dy = -1; dy <= 1; dy++) {
            Cell c = w->cells[world_index(w, x, row + dy)];
            stirred += g_elem_behavior[c.element] == BEHAVIOR_GAS && gas_wind_speed(c.aux) > 0;
        }
    }
    EXPECT(stirred > 50);
    world_destroy(w);
}

void test_shooting_same_for_any_thread_count(void) {
    // Shots, debris and casings are serial: bit-identical on 1 and 4 threads.
    JobPool *pool = jobs_create(4);
    int pa, pb;
    World *a = range(104, ELEM_DIRT, &pa), *b = range(104, ELEM_DIRT, &pb);
    world_set_job_pool(b, pool);
    shoot(a, pa, 100.0f, -10.0f, 8, 120);
    shoot(b, pb, 100.0f, -10.0f, 8, 120);
    size_t n = (size_t)a->width * a->height;
    EXPECT(memcmp(a->cells, b->cells, n * sizeof(Cell)) == 0);
    EXPECT(memcmp(a->temp, b->temp, n * sizeof(float)) == 0);
    EXPECT(memcmp(a->gas, b->gas, n * sizeof(GasMix)) == 0);
    world_destroy(a);
    world_destroy(b);
    jobs_destroy(pool);
}

void test_small_clumps_fall(void) {
    // Digging one cell of a small floating clump (20 cells or less) loosens all of it, so it
    // falls; a bigger one stays, and so does a small one held by iron.
    World *w = test_world_box(128, 128, 105);
    int64_t walls = test_count(w, ELEM_DIRT);
    fill(w, 10, 10, 13, 13, ELEM_DIRT); // 16 cells, floating
    fill(w, 30, 10, 35, 14, ELEM_ROCK); // 30 cells, floating
    fill(w, 50, 10, 53, 13, ELEM_DIRT); // 16 cells, on an iron post
    fill(w, 51, 14, 51, 30, ELEM_IRON);
    Edit dig = {10, 10, 10, 10, 0, EDIT_SQUARE, ELEM_VOID, 0, EDIT_DIG}; // a corner of each
    sim_apply_edit(w, &dig);
    dig.x0 = dig.x1 = 30;
    sim_apply_edit(w, &dig);
    dig.x0 = dig.x1 = 50;
    sim_apply_edit(w, &dig);
    EXPECT_EQ(test_count(w, ELEM_SOIL), 16 + 1);     // the floating clump, and one dug cell
    EXPECT_EQ(test_count(w, ELEM_DIRT) - walls, 15); // the held clump
    EXPECT_EQ(test_count(w, ELEM_ROCK), 29);         // too big
    EXPECT_EQ(test_count(w, ELEM_RUBBLE), 1);
    sim_run_ticks(w, 300);
    EXPECT(test_mean_y(w, ELEM_SOIL) > 110.0); // fell to the floor
    world_destroy(w);
}

void test_player_runs_and_jumps(void) {
    // The player is 1.85 m tall (37 cells) and runs at 4 m/s (80 cells/s); a jump lifts the body
    // about 50 cm (10 cells).
    World *w = test_world_box(512, 128, 106);
    fill(w, 1, 1, 510, 126, EDIT_AIR);
    int pl = scene_spawn_player(w, 40);
    EXPECT(pl >= 0);
    Creature *c = &w->creatures[pl];
    EXPECT_EQ(c->h, 37);
    sim_run_ticks(w, 30);
    EXPECT(c->grounded);
    int x0 = c->x, y0 = c->y;
    c->walk = 1;
    sim_run_ticks(w, 60);
    EXPECT(abs(c->x - x0 - 80) <= 3);
    c->walk = 0;
    sim_run_ticks(w, 10);
    c->jump = 1;
    int top = c->y;
    for (int t = 0; t < 60; t++) {
        sim_tick(w, NULL);
        top = PR_MIN(top, c->y);
    }
    EXPECT(abs((y0 - top) - 10) <= 2);
    EXPECT_EQ(c->y, y0); // back on the floor
    world_destroy(w);
}

void test_air_brush_spreads_oxygen(void) {
    // The Air brush paints the same mixture in every cell (20.95% oxygen by moles), so painted
    // air does not move: a big room of it is asleep at once.
    World *w = test_world_box(256, 256, 107);
    fill(w, 1, 1, 254, 254, EDIT_AIR);
    double o2 = test_mass(w, ELEM_OXYGEN) / (double)g_species_molar[GAS_O2];
    double all = 0.0;
    for (int s = 0; s < GAS_SPECIES; s++) {
        all += test_mass(w, g_species_elem[s]) / (double)g_species_molar[s];
    }
    EXPECT(fabs(o2 / all - (double)AIR_O2_FRAC) < 1e-4);
    sim_run_ticks(w, 2);
    SimStats st;
    sim_tick(w, &st);
    EXPECT(st.cells_visited < 100);
    world_destroy(w);
}

// A long room of air (512 x 128 world), the player on the floor at the left.
static World *corridor(uint64_t seed, int *player) {
    World *w = test_world_box(512, 128, seed);
    fill(w, 1, 1, 510, 126, EDIT_AIR);
    *player = scene_spawn_player(w, 30);
    sim_run_ticks(w, 30);
    return w;
}

void test_walk_forward_while_shooting(void) {
    // Running right while firing ahead (2 s, 24 shots): the casings thrown back through the player
    // do not stop it, and neither do the ones on the ground when it runs back. It covers most of
    // the distance it runs without shooting (80 cells in 1 s), and every casing and bullet is
    // still in the world.
    int pl;
    World *w = corridor(108, &pl);
    Creature *c = &w->creatures[pl];
    int x0 = c->x;
    Rifle r = {0};
    c->walk = 1;
    for (int t = 0; t < 120; t++) {
        float sx, sy;
        rifle_shoulder(c, &sx, &sy);
        rifle_update(w, &r, pl, sx + 200.0f, sy - 5.0f, true);
        sim_tick(w, NULL);
    }
    EXPECT(c->x - x0 > 150); // 160 without shooting
    // Then back the other way, still firing right: into its own casings, now on the ground.
    int x1 = c->x;
    c->walk = -1;
    for (int t = 0; t < 120; t++) {
        float sx, sy;
        rifle_shoulder(c, &sx, &sy);
        rifle_update(w, &r, pl, sx + 200.0f, sy - 5.0f, true);
        sim_tick(w, NULL);
    }
    EXPECT(x1 - c->x > 150);
    c->walk = 0;
    sim_run_ticks(w, 300);
    EXPECT_EQ(test_count(w, ELEM_CASING), (int64_t)r.shots);
    EXPECT_EQ(test_count(w, ELEM_BULLET), (int64_t)r.shots);
    world_destroy(w);
}

void test_walking_kicks_small_piles(void) {
    // Small piles on the floor (casings, bullets, loose soil, rubble; up to 30 cm high) are kicked
    // away by a walker instead of stopping it; nothing is destroyed.
    int pl;
    World *w = corridor(109, &pl);
    fill(w, 60, 121, 63, 126, ELEM_CASING);
    fill(w, 100, 123, 105, 126, ELEM_BULLET);
    fill(w, 140, 120, 144, 126, ELEM_SOIL);
    fill(w, 180, 122, 186, 126, ELEM_RUBBLE);
    int64_t n[4] = {test_count(w, ELEM_CASING), test_count(w, ELEM_BULLET),
                    test_count(w, ELEM_SOIL), test_count(w, ELEM_RUBBLE)};
    Creature *c = &w->creatures[pl];
    int y0 = c->y, top = c->y;
    c->walk = 1;
    for (int t = 0; t < 180; t++) {
        sim_tick(w, NULL);
        top = PR_MIN(top, c->y);
    }
    EXPECT(c->x > 220);
    // It kicked them away rather than climbing over them (it climbed 5 cells before kicking);
    // what it kicks ahead may land on the next pile and make it too tall to kick (40 cm): then
    // it steps up a little.
    EXPECT(y0 - top <= 3);
    c->walk = 0;
    sim_run_ticks(w, 300);
    EXPECT_EQ(test_count(w, ELEM_CASING), n[0]);
    EXPECT_EQ(test_count(w, ELEM_BULLET), n[1]);
    EXPECT_EQ(test_count(w, ELEM_SOIL), n[2]);
    EXPECT_EQ(test_count(w, ELEM_RUBBLE), n[3]);
    EXPECT_EQ(w->particle_count, 0);
    world_destroy(w);
}

void test_walker_crosses_heaps(void) {
    // Heaps of loose soil 1 and 2 m high (slumped to 45 degrees) are climbed, not dug into: the
    // walker kicks only small piles, so it never plows into a heap until the face is too high,
    // and what it kicks does not land on a slope ahead and slide back against it. It crosses each
    // heap, and back again.
    static const int k_heights[] = {20, 40};
    for (int i = 0; i < 2; i++) {
        World *w = test_world_box(256, 128, 112 + (uint64_t)i);
        fill(w, 1, 1, 254, 126, EDIT_AIR);
        fill(w, 118, 126 - k_heights[i], 138, 126, ELEM_SOIL);
        sim_run_ticks(w, 240); // slumps into a heap
        int pl = scene_spawn_player(w, 220);
        EXPECT(pl >= 0);
        Creature *c = &w->creatures[pl];
        c->walk = -1;
        sim_run_ticks(w, 300);
        EXPECT(c->x < 50);
        c->walk = 1;
        sim_run_ticks(w, 300);
        EXPECT(c->x > 190);
        world_destroy(w);
    }
}

void test_falling_powder_does_not_block_walker(void) {
    // Powder in mid-air (falling, or lying on a body) never blocks a body: walking into a falling
    // column of loose soil, or jumping with soil on the head, the body lifts it out of its way; it
    // falls through the body and lands on the ground. Nothing is lost.
    World *w = test_world_box(192, 128, 115);
    fill(w, 1, 1, 190, 126, EDIT_AIR);
    int pl = scene_spawn_player(w, 60);
    Creature *c = &w->creatures[pl];
    sim_run_ticks(w, 20);
    EXPECT(c->grounded);
    int x0 = c->x;
    // A column of soil falling just in front of the body, from its head down to 10 cells above
    // the floor.
    int fx = c->x + c->w;
    for (int y = c->y; y < c->y + c->h - 10; y++) {
        world_set_cell(w, fx, y, ELEM_SOIL, (uint32_t)y);
        w->cells[world_index(w, fx, y)].aux = 16; // falling, one cell per tick
    }
    int64_t soil = test_count(w, ELEM_SOIL);
    c->walk = 1;
    sim_tick(w, NULL);
    EXPECT(c->x > x0); // not stopped by it
    sim_run_ticks(w, 5);
    c->walk = 0;
    sim_run_ticks(w, 120);
    EXPECT_EQ(test_count(w, ELEM_SOIL), soil);
    EXPECT_EQ(w->particle_count, 0); // all of it landed
    // Soil lying on its head: it still jumps its full height.
    int y1 = c->y;
    for (int k = 0; k < c->w; k++) {
        world_set_cell(w, c->x + k, c->y - 1, ELEM_SOIL, (uint32_t)k);
    }
    c->jump = 1;
    int top = c->y;
    for (int t = 0; t < 60; t++) {
        sim_tick(w, NULL);
        top = PR_MIN(top, c->y);
    }
    EXPECT(y1 - top >= 8);
    EXPECT(abs(c->y - y1) <= 3); // landed again (perhaps on a little of the soil)
    sim_run_ticks(w, 60);
    EXPECT_EQ(test_count(w, ELEM_SOIL), soil + c->w);
    world_destroy(w);
}

void test_player_steers_in_the_air(void) {
    // Jumping from standing still, the player can still move sideways in the air (air control).
    World *w = test_world_box(192, 128, 116);
    fill(w, 1, 1, 190, 126, EDIT_AIR);
    int pl = scene_spawn_player(w, 60);
    Creature *c = &w->creatures[pl];
    sim_run_ticks(w, 20);
    int x0 = c->x;
    c->jump = 1;
    sim_tick(w, NULL);
    EXPECT(!c->grounded);
    c->walk = 1;
    sim_run_ticks(w, 20);
    EXPECT(!c->grounded); // still in the air
    EXPECT(c->x - x0 >= 12);
    world_destroy(w);
}

void test_muzzle_gas(void) {
    // Each shot blows the propellant out of the muzzle as hot gas (1.6 g: CO2, water vapour and
    // nitrogen, at 1500 K, mixed into the air there). It glows for a moment, and its current mixes
    // it (and its heat) into the air within a fraction of a second.
    int pl;
    World *w = range(117, ELEM_ROCK, &pl);
    double co2_0 = test_mass(w, ELEM_CO2);
    Rifle r = {0};
    float sx, sy;
    rifle_shoulder(&w->creatures[pl], &sx, &sy);
    EXPECT(rifle_update(w, &r, pl, sx + 100.0f, sy, true));
    int muzzle_x = (int)sx + RIFLE_LENGTH;
    int hot = 0;
    for (int y = (int)sy - 4; y <= (int)sy + 4; y++) {
        for (int x = muzzle_x - 2; x <= muzzle_x + 8; x++) {
            ElementId e = world_elem(w, x, y);
            hot += g_elem_behavior[e] == BEHAVIOR_GAS &&
                   elem_temp(e, w->temp[world_index(w, x, y)]) > 1000.0f;
        }
    }
    EXPECT(hot >= 4); // the CO2 cells (the nitrogen mixes into the air's at once)
    EXPECT(fabs(test_mass(w, ELEM_CO2) - co2_0 - 0.88e-3) < 0.01e-3);
    EXPECT(test_mass(w, ELEM_STEAM) > 0.42e-3); // water vapour too
    sim_run_ticks(w, 20);
    float hottest = 0.0f;
    for (int i = 0; i < w->width * w->height; i++) {
        ElementId e = w->cells[i].element;
        if (g_elem_behavior[e] == BEHAVIOR_GAS) {
            hottest = PR_MAX(hottest, elem_temp(e, w->temp[i]));
        }
    }
    EXPECT(hottest < 750.0f); // no longer glowing
    EXPECT(hottest > 320.0f); // but the air in front is still warm
    world_destroy(w);
}

void test_bullets_and_casings_melt(void) {
    // Spent bullets (lead, melting at 327 degC) and casings (brass, about 935 degC) dropped into
    // lava melt; the molten metals are denser than lava and sink to the bottom. Cooled, they
    // solidify into lead and brass. Every cell is kept.
    World *w = test_world_box(64, 64, 118);
    fill(w, 1, 30, 62, 62, ELEM_LAVA);
    fill(w, 20, 26, 29, 27, ELEM_BULLET);
    fill(w, 34, 26, 43, 27, ELEM_CASING);
    int64_t nb = test_count(w, ELEM_BULLET), nc = test_count(w, ELEM_CASING);
    EXPECT_EQ(nb, 20);
    sim_run_ticks(w, 600);
    EXPECT_EQ(test_count(w, ELEM_BULLET), 0);
    EXPECT_EQ(test_count(w, ELEM_CASING), 0);
    EXPECT_EQ(test_count(w, ELEM_MOLTEN_LEAD) + test_count(w, ELEM_LEAD), nb);
    EXPECT_EQ(test_count(w, ELEM_MOLTEN_BRASS) + test_count(w, ELEM_BRASS), nc);
    EXPECT(test_mean_y(w, ELEM_MOLTEN_LEAD) > 55.0); // at the bottom
    // Cooled: everything at 20 degC.
    for (int i = 0; i < w->width * w->height; i++) {
        ElementId e = w->cells[i].element;
        if (e == ELEM_MOLTEN_LEAD || e == ELEM_MOLTEN_BRASS || e == ELEM_LAVA) {
            w->temp[i] = 293.15f;
        }
    }
    world_wake_heat(w, 0, 0, 63, 63);
    sim_run_ticks(w, 10);
    EXPECT_EQ(test_count(w, ELEM_LEAD), nb);
    EXPECT_EQ(test_count(w, ELEM_BRASS), nc);
    world_destroy(w);
}
