#include "test.h"

#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/enemy.h"
#include "sim/particle.h"
#include "sim/pickup.h"
#include "sim/rigid.h"
#include "sim/scene.h"
#include "sim/sim.h"
#include "sim/weapon.h"

#include <math.h>
#include <stdlib.h>

void test_rifle_hurts_and_monsters_bleed(void);
void test_monsters_chase_and_bite(void);
void test_rifle_ammunition_and_reload(void);
void test_crates_are_picked_up(void);
void test_level_has_crates_and_monsters(void);
void test_monsters_lose_pixels_with_hit_points(void);
void test_monster_dies_cut_in_two_or_half_gone(void);
void test_corpse_is_a_rigid_body_shot_apart(void);
void test_monsters_chase_until_off_screen(void);
void test_walker_finds_the_hole_down(void);
void test_flyer_finds_the_gap(void);

static World *room(uint64_t seed, int width) {
    World *w = test_world_box(width, 128, seed);
    for (int y = 1; y <= 126; y++) {
        Edit air = {1, y, width - 2, y, 0, EDIT_SQUARE, EDIT_AIR, 0, EDIT_PAINT};
        sim_apply_edit(w, &air);
    }
    return w;
}

void test_rifle_hurts_and_monsters_bleed(void) {
    // A bullet takes RIFLE_DAMAGE from a walker (160 hp) and blood flies from it. A kill turns it
    // into a rigid body that lies on the floor and bleeds on until its blood (1.5 droplets a pixel
    // left) is gone.
    World *w = room(130, 256);
    int pl = scene_spawn_player(w, 40);
    int mi = enemy_spawn(w, KIND_WALKER, 210, 126 - WALKER_H + 1);
    EXPECT(pl >= 0 && mi >= 0);
    sim_run_ticks(w, 20);
    Creature *m = &w->creatures[mi];
    EXPECT_EQ(test_count(w, ELEM_BLOOD), 0);
    Rifle r = {0};
    for (int t = 0; t < 60 && m->hp >= WALKER_HP; t++) {
        rifle_update(w, &r, pl, (float)m->x + (float)m->w * 0.5f, (float)m->y + (float)m->h * 0.5f,
                     t == 0);
        sim_tick(w, NULL);
    }
    EXPECT(fabsf(m->hp - (WALKER_HP - RIFLE_DAMAGE)) < 1e-3f); // one shot, one hit
    EXPECT_EQ(w->creatures[pl].hp, PERSON_HP); // its own bullets do not hurt the player
    EXPECT(m->state != CREATURE_DEAD);
    EXPECT(test_count(w, ELEM_BLOOD) > 5);
    // Finished off: it dies and becomes a rigid body (what is left of it), which lies on the
    // floor and bleeds.
    creature_hit(w, m, 400.0f, m->x - 1, m->y + 2, 1.0f, 0.0f, m->x, m->y + 2);
    EXPECT(m->state == CREATURE_DEAD && m->corpse);
    EXPECT(m->hp == 0.0f);
    EXPECT_EQ(test_count(w, ELEM_BODY), 85); // only the player's (5 x 17 cells) is left
    int64_t at_death = test_count(w, ELEM_BLOOD);
    sim_run_ticks(w, 300);
    int64_t bled = test_count(w, ELEM_BLOOD) - at_death;
    EXPECT(bled >= 20 && bled <= (int64_t)(1.5f * WALKER_W * WALKER_H));
    int bodies = 0;
    for (int i = 0; i < w->rigid_count; i++) {
        if (w->rigids[i].active) {
            bodies++;
            EXPECT(w->rigids[i].asleep);
            EXPECT(w->rigids[i].y > 120.0f); // lying on the floor
            EXPECT(w->rigids[i].bleed <= 0.5f);
        }
    }
    EXPECT_EQ(bodies, 1);
    EXPECT(test_count(w, ELEM_WALKER_CORPSE) >= 18);
    world_destroy(w);
}

void test_monsters_chase_and_bite(void) {
    // Within range and in sight a walker runs at the player and a flyer flies at it; both bite
    // when they touch it. Out of range they stay away.
    World *w = room(131, 512);
    int pl = scene_spawn_player(w, 40);
    int wi = enemy_spawn(w, KIND_WALKER, 140, 126 - WALKER_H + 1);
    int fi = enemy_spawn(w, KIND_FLYER, 150, 30);
    int far_i = enemy_spawn(w, KIND_WALKER, 460, 126 - WALKER_H + 1);
    EXPECT(pl >= 0 && wi >= 0 && fi >= 0 && far_i >= 0);
    Creature *p = &w->creatures[pl];
    sim_run_ticks(w, 240);
    const Creature *walker = &w->creatures[wi], *flyer = &w->creatures[fi];
    EXPECT(walker->x - (p->x + p->w) <= 3);
    EXPECT(abs(flyer->x - p->x) <= 14 && abs(flyer->y - p->y) <= 14);
    EXPECT(p->hp < PERSON_HP - 15.0f); // bitten by both
    EXPECT(p->hp > 0.0f);
    EXPECT(w->creatures[far_i].x > 300); // never saw it
    world_destroy(w);
}

void test_rifle_ammunition_and_reload(void) {
    World *w = room(132, 192);
    int pl = scene_spawn_player(w, 40);
    sim_run_ticks(w, 20);
    Rifle r;
    rifle_init(&r);
    EXPECT_EQ(r.mag, RIFLE_MAG);
    EXPECT_EQ(r.reserve, RIFLE_RESERVE_START);
    EXPECT(!rifle_reload(&r)); // the magazine is full
    float sx, sy;
    rifle_shoulder(&w->creatures[pl], &sx, &sy);
    for (int t = 0; t < 400 && r.shots < RIFLE_MAG + 5; t++) {
        rifle_update(w, &r, pl, sx + 100.0f, sy, true);
        sim_tick(w, NULL);
        if (r.shots == RIFLE_MAG && r.reload == 0 && r.mag == 0) {
            break;
        }
    }
    EXPECT_EQ(r.shots, RIFLE_MAG);
    EXPECT_EQ(r.mag, 0);
    // The trigger on an empty magazine reloads; nothing fires meanwhile.
    for (int t = 0; t < RIFLE_RELOAD_TICKS + 2; t++) {
        rifle_update(w, &r, pl, sx + 100.0f, sy, t < 10);
        sim_tick(w, NULL);
    }
    EXPECT_EQ(r.shots, RIFLE_MAG);
    EXPECT_EQ(r.mag, RIFLE_MAG);
    EXPECT_EQ(r.reserve, RIFLE_RESERVE_START - RIFLE_MAG);
    // A partial magazine is topped up from the pack (R).
    r.mag = 25;
    EXPECT(rifle_reload(&r));
    EXPECT(!rifle_reload(&r)); // already reloading
    for (int t = 0; t < RIFLE_RELOAD_TICKS; t++) {
        rifle_update(w, &r, pl, sx + 100.0f, sy, false);
    }
    EXPECT_EQ(r.mag, RIFLE_MAG);
    EXPECT_EQ(r.reserve, RIFLE_RESERVE_START - RIFLE_MAG - 5);
    // An empty pack: nothing to reload with.
    r.mag = 3;
    r.reserve = 0;
    EXPECT(!rifle_reload(&r));
    world_destroy(w);
}

void test_crates_are_picked_up(void) {
    World *w = room(133, 192);
    int pl = scene_spawn_player(w, 40);
    sim_run_ticks(w, 20);
    Creature *p = &w->creatures[pl];
    Rifle r;
    rifle_init(&r);
    r.reserve = 10;
    int cx = p->x + p->w / 2;
    // A crate dropped from the air falls to the floor.
    int a = pickup_add(w, PICKUP_AMMO, 120, 40);
    sim_run_ticks(w, 120);
    EXPECT_EQ(w->pickups[a].y + w->pickups[a].h - 1, 126);
    EXPECT_EQ(pickups_collect(w, pl, &r), 0); // not touching it
    // Standing on an ammunition crate takes it; not when the pack is full.
    int b = pickup_add(w, PICKUP_AMMO, cx, 126);
    EXPECT_EQ(pickups_collect(w, pl, &r), 1);
    EXPECT(w->pickups[b].taken);
    EXPECT_EQ(r.reserve, 10 + PICKUP_AMMO_ROUNDS);
    r.reserve = RIFLE_RESERVE_MAX;
    int c = pickup_add(w, PICKUP_AMMO, cx, 126);
    EXPECT_EQ(pickups_collect(w, pl, &r), 0);
    EXPECT(!w->pickups[c].taken);
    // A health crate heals (up to the maximum), and stays if the player is not hurt.
    int d = pickup_add(w, PICKUP_HEALTH, cx, 126);
    EXPECT_EQ(pickups_collect(w, pl, &r), 0);
    p->hp = 50.0f;
    EXPECT_EQ(pickups_collect(w, pl, &r), 1);
    EXPECT(w->pickups[d].taken);
    EXPECT(fabsf(p->hp - (50.0f + PICKUP_HEALTH_HP)) < 1e-3f);
    world_destroy(w);
}

void test_level_has_crates_and_monsters(void) {
    int walkers[2], flyers[2], ammo[2], health[2];
    for (int run = 0; run < 2; run++) {
        World *w = world_create(2048, 512, 134);
        scene_level(w);
        walkers[run] = flyers[run] = ammo[run] = health[run] = 0;
        for (int i = 0; i < w->creature_count; i++) {
            const Creature *c = &w->creatures[i];
            walkers[run] += c->kind == KIND_WALKER;
            flyers[run] += c->kind == KIND_FLYER;
            EXPECT(test_cell_is(w, c->x, c->y, ELEM_BODY));
        }
        for (int i = 0; i < w->pickup_count; i++) {
            const Pickup *p = &w->pickups[i];
            ammo[run] += p->kind == PICKUP_AMMO;
            health[run] += p->kind == PICKUP_HEALTH;
            for (int k = 0; k < p->w; k++) { // in open space, on the ground
                EXPECT(g_elem_behavior[world_elem(w, p->x + k, p->y)] == BEHAVIOR_GAS);
                EXPECT(!particle_passes(w, p->x + k, p->y + p->h));
            }
        }
        world_destroy(w);
    }
    EXPECT(walkers[0] > 0 && flyers[0] > 0 && ammo[0] > 0 && health[0] > 0);
    EXPECT_EQ(walkers[0], walkers[1]); // deterministic
    EXPECT_EQ(flyers[0], flyers[1]);
    EXPECT_EQ(ammo[0], ammo[1]);
    EXPECT_EQ(health[0], health[1]);
}

void test_monsters_chase_until_off_screen(void) {
    // A walker that has seen the player keeps chasing when a wall comes between them (no
    // pathfinding: it pushes against it), until the player is off the screen.
    World *w = room(135, 512);
    int pl = scene_spawn_player(w, 40);
    int wi = enemy_spawn(w, KIND_WALKER, 200, 126 - WALKER_H + 1);
    int si = enemy_spawn(w, KIND_WALKER, 300, 126 - WALKER_H + 1); // starts behind a wall
    EXPECT(pl >= 0 && wi >= 0 && si >= 0);
    Creature *seer = &w->creatures[wi], *blind = &w->creatures[si];
    for (int y = 1; y <= 126; y++) { // a wall of rock at x = 250
        Edit rock = {250, y, 252, y, 0, EDIT_SQUARE, ELEM_ROCK, 0, EDIT_PAINT};
        sim_apply_edit(w, &rock);
    }
    sim_run_ticks(w, 10);
    EXPECT(seer->alert);
    EXPECT(!blind->alert); // it does not see the player through the wall
    sim_run_ticks(w, 60);
    EXPECT(seer->alert);
    // Then a wall between: it goes on chasing, pressing toward the player.
    for (int y = 1; y <= 126; y++) {
        Edit rock = {120, y, 122, y, 0, EDIT_SQUARE, ELEM_ROCK, 0, EDIT_PAINT};
        sim_apply_edit(w, &rock);
    }
    sim_run_ticks(w, 100);
    EXPECT(seer->alert);
    EXPECT(seer->x >= 120 && seer->x < 160); // against the wall, not wandering off
    // The screen shrinks to less than the distance: it gives up.
    w->view_w = 100;
    sim_run_ticks(w, 2);
    EXPECT(!seer->alert);
    world_destroy(w);
}

static void rock(World *w, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; y++) {
        Edit e = {x0, y, x1, y, 0, EDIT_SQUARE, ELEM_ROCK, 0, EDIT_PAINT};
        sim_apply_edit(w, &e);
    }
}

void test_walker_finds_the_hole_down(void) {
    // The player is in a room under a slab, a walker on top of it (alert): it walks to the hole in
    // the slab, drops through, and comes back along the floor to the player. Without a route it
    // would press along the slab above the player for ever.
    World *w = room(136, 512);
    rock(w, 1, 60, 510, 66);
    for (int y = 60; y <= 66; y++) { // the hole
        Edit hole = {150, y, 200, y, 0, EDIT_SQUARE, EDIT_AIR, 0, EDIT_PAINT};
        sim_apply_edit(w, &hole);
    }
    int pl = scene_spawn_player_from(w, 40, 80);
    int wi = enemy_spawn(w, KIND_WALKER, 100, 60 - WALKER_H);
    EXPECT(pl >= 0 && wi >= 0);
    Creature *m = &w->creatures[wi];
    m->alert = 1;
    sim_run_ticks(w, 450);
    EXPECT(m->y > 100); // it came down
    EXPECT(m->x < 110);  // and back toward the player
    EXPECT(w->creatures[pl].hp < PERSON_HP); // and bit it
    world_destroy(w);
}

void test_flyer_finds_the_gap(void) {
    // A wall with a gap at the top between the flyer (alert) and the player: it flies up, over,
    // and down to the player instead of pressing against the wall.
    World *w = room(137, 512);
    int pl = scene_spawn_player(w, 40);
    int fi = enemy_spawn(w, KIND_FLYER, 200, 100);
    EXPECT(pl >= 0 && fi >= 0);
    rock(w, 120, 30, 127, 126); // thick enough for the 6-cell blocks to see (path.h)
    Creature *m = &w->creatures[fi];
    m->alert = 1;
    sim_run_ticks(w, 500);
    EXPECT(m->x < 100);
    EXPECT(w->creatures[pl].hp < PERSON_HP);
    world_destroy(w);
}

static int active_bodies(const World *w) {
    int n = 0;
    for (int i = 0; i < w->rigid_count; i++) {
        n += w->rigids[i].active;
    }
    return n;
}

void test_monsters_lose_pixels_with_hit_points(void) {
    // A monster loses as many pixels as it is wide in all, in proportion to the hit points it
    // loses (a walker, 6 wide, of 160 hp: a pixel every 26.7 hp), first the pixel that was hit.
    EXPECT_EQ((int)WALKER_HP, 160); // four times what they had
    EXPECT_EQ((int)FLYER_HP, 120);
    World *w = room(138, 128);
    int mi = enemy_spawn(w, KIND_WALKER, 60, 126 - WALKER_H + 1);
    EXPECT(mi >= 0);
    Creature *m = &w->creatures[mi];
    EXPECT_EQ(monster_pixels(w, m), 36);
    int hx = m->x + 2, hy = m->y + 3;
    creature_hit(w, m, 40.0f, hx - 3, hy, 1.0f, 0.0f, hx, hy); // 1.5 pixels owed: one lost
    EXPECT_EQ(monster_pixels(w, m), 35);
    EXPECT(world_elem(w, hx, hy) != ELEM_BODY); // the one that was hit
    creature_hit(w, m, 40.0f, hx - 3, hy, 1.0f, 0.0f, hx, hy + 1); // 3 owed in all: 3 lost
    EXPECT_EQ(monster_pixels(w, m), 33);
    creature_hit(w, m, 40.0f, hx - 3, hy, 1.0f, 0.0f, hx + 1, hy); // 4.5 owed: 4 lost
    EXPECT_EQ(monster_pixels(w, m), 32);
    EXPECT(m->state != CREATURE_DEAD);
    creature_hit(w, m, 40.0f, hx - 3, hy, 1.0f, 0.0f, hx, hy - 1); // hp 0: 6 lost in all, dead
    EXPECT(m->state == CREATURE_DEAD && m->corpse);
    EXPECT_EQ(test_count(w, ELEM_WALKER_CORPSE), 30);
    EXPECT_EQ(active_bodies(w), 1);
    // A monster of 6 hp as wide as 6 loses a pixel for each hp lost.
    int ni = enemy_spawn(w, KIND_WALKER, 90, 126 - WALKER_H + 1);
    Creature *n = &w->creatures[ni];
    n->hp = n->hp_max = 6.0f;
    static const int8_t k_hit[6][2] = {{0, 0}, {5, 5}, {0, 5}, {5, 0}, {2, 2}, {3, 3}};
    for (int k = 0; k < 6; k++) {
        EXPECT_EQ(monster_pixels(w, n), 36 - k);
        creature_hit(w, n, 1.0f, n->x - 1, n->y, 1.0f, 0.0f, n->x + k_hit[k][0], n->y + k_hit[k][1]);
    }
    EXPECT(n->corpse); // hp 0 after the sixth: dead, 6 pixels lighter
    world_destroy(w);
}

void test_monster_dies_cut_in_two_or_half_gone(void) {
    // Cut in two (a whole row of pixels gone), a monster dies at once, and its corpse is two rigid
    // bodies; so does one that has lost over half its pixels.
    World *w = room(139, 128);
    int mi = enemy_spawn(w, KIND_WALKER, 40, 126 - WALKER_H + 1);
    Creature *m = &w->creatures[mi];
    for (int k = 0; k < 6; k++) {
        EXPECT(monster_lose_pixel(w, m, m->x + k, m->y + 3));
    }
    EXPECT(m->state != CREATURE_DEAD);
    creature_hit(w, m, 0.0f, m->x, m->y, 1.0f, 0.0f, -1, -1); // checks the body
    EXPECT(m->corpse);
    EXPECT_EQ(active_bodies(w), 2);
    EXPECT_EQ(test_count(w, ELEM_WALKER_CORPSE), 30);
    int ni = enemy_spawn(w, KIND_WALKER, 80, 126 - WALKER_H + 1);
    Creature *n = &w->creatures[ni];
    for (int k = 0; k < 19; k++) { // rows 0 to 2 and a pixel of row 3: 17 of 36 left
        EXPECT(monster_lose_pixel(w, n, n->x + k % 6, n->y + k / 6));
    }
    EXPECT(n->state != CREATURE_DEAD);
    creature_hit(w, n, 0.0f, n->x, n->y, 1.0f, 0.0f, -1, -1);
    EXPECT(n->corpse);
    EXPECT_EQ(test_count(w, ELEM_WALKER_CORPSE), 30 + 17);
    world_destroy(w);
}

void test_corpse_is_a_rigid_body_shot_apart(void) {
    // A flyer killed in the air falls as a rigid body, lands, and sleeps; a bullet through it
    // cuts it in two, with blood where it goes in.
    World *w = room(140, 128);
    int fi = enemy_spawn(w, KIND_FLYER, 60, 40);
    EXPECT(fi >= 0);
    Creature *f = &w->creatures[fi];
    creature_hit(w, f, 1000.0f, f->x - 1, f->y + 4, 1.0f, 0.0f, f->x + 5, f->y + 5);
    EXPECT(f->corpse);
    EXPECT_EQ(active_bodies(w), 1);
    int pixels = test_count(w, ELEM_FLYER_CORPSE);
    EXPECT_EQ(pixels, 121 - 11);
    sim_run_ticks(w, 200);
    const Rigid *r = &w->rigids[0];
    EXPECT(r->active && r->asleep);
    EXPECT(r->y > 118.0f); // fell to the floor
    EXPECT(test_count(w, ELEM_FLYER_CORPSE) > 90);
    int64_t blood = test_count(w, ELEM_BLOOD);
    Cell bullet = {ELEM_BULLET, 0, 0, 0};
    Particle b = {r->x, 60.5f, 0.0f, 300.0f, bullet, T_ROOM};
    EXPECT(particle_add(w, &b));
    sim_run_ticks(w, 30);
    EXPECT(active_bodies(w) >= 2);              // cut in two
    EXPECT(test_count(w, ELEM_BLOOD) > blood);  // bleeding where it was hit
    world_destroy(w);
}
