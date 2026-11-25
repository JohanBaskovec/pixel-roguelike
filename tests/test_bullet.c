#include "test.h"

#include "sim/bullet.h"
#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/enemy.h"
#include "sim/scene.h"
#include "sim/sim.h"
#include "sim/weapon.h"

#include <math.h>

void test_bullet_design_rules(void);
void test_bullet_releases_gas_gradually_or_on_impact(void);
void test_bullet_liquid_and_heat(void);
void test_example_bomb_bullet_burns(void);
void test_bullet_damage_comes_from_solids(void);

// A room of air (192 x 128) with a wall of rock at x >= 160 and the player standing at the left.
static World *range(uint64_t seed, int *player) {
    World *w = test_world_box(192, 128, seed);
    for (int y = 1; y <= 126; y++) {
        Edit air = {1, y, 190, y, 0, EDIT_SQUARE, EDIT_AIR, 0, EDIT_PAINT, 0.0f};
        sim_apply_edit(w, &air);
        Edit rock = {160, y, 190, y, 0, EDIT_SQUARE, ELEM_ROCK, 0, EDIT_PAINT, 0.0f};
        sim_apply_edit(w, &rock);
    }
    *player = scene_spawn_player(w, 40);
    sim_run_ticks(w, 20);
    return w;
}

static void put(BulletDesign *d, int i, int kind, ElementId e, int release, float kpa, float pct,
                float temp_k) {
    d->px[i] = (BulletPixel){(uint8_t)kind, e, (uint8_t)release, kpa, pct, temp_k};
}

// Fires one bullet of design d straight along the line of sight, `distance` cells (the wall is
// at x = 160: the player is at 40), and runs `ticks` ticks.
static void shoot(World *w, int pl, const BulletDesign *d, int ticks) {
    Rifle r = {0};
    r.nslots = 1;
    r.slots[0] = *d;
    float sx, sy;
    rifle_shoulder(&w->creatures[pl], &sx, &sy);
    EXPECT(rifle_update(w, &r, pl, sx + 100.0f, sy, true));
    sim_run_ticks(w, ticks);
}

void test_bullet_design_rules(void) {
    elements_init();
    BulletDesign d;
    bullet_design_clear(&d);
    EXPECT(!bullet_design_connected(&d)); // empty
    put(&d, 0, BULLET_MATTER, ELEM_IRON, 0, 0, 0, 0);
    EXPECT(bullet_design_connected(&d));
    put(&d, 1, BULLET_MATTER, ELEM_HYDROGEN, RELEASE_IMPACT, 300.0f, 0, 0);
    EXPECT(bullet_design_connected(&d)); // side by side
    put(&d, 3, BULLET_HEAT, ELEM_VOID, RELEASE_IMPACT, 0, 0, 2273.15f);
    EXPECT(!bullet_design_connected(&d)); // a gap: two pieces
    put(&d, 2, BULLET_MATTER, ELEM_OXYGEN, RELEASE_IMPACT, 100.0f, 0, 0);
    EXPECT(bullet_design_connected(&d));
    put(&d, BULLET_DIM + 5, BULLET_MATTER, ELEM_DIRT, 0, 0, 0, 0); // touches nothing (diagonals do not count)
    EXPECT(!bullet_design_connected(&d));
    // The starting bullet: an iron pixel, the damage of a rifle bullet.
    bullet_design_iron(&d);
    EXPECT(fabsf(bullet_design_damage(&d) - RIFLE_DAMAGE) < 1e-3f);
    EXPECT(bullet_design_has_solid(&d) && !bullet_design_has_payload(&d));
    EXPECT(bullet_element_allowed(ELEM_WATER) && bullet_element_allowed(ELEM_HYDROGEN) &&
           bullet_element_allowed(ELEM_ROCK));
    EXPECT(!bullet_element_allowed(ELEM_BODY) && !bullet_element_allowed(ELEM_VOID));
}

void test_bullet_releases_gas_gradually_or_on_impact(void) {
    // A pixel of hydrogen at 300 kPa: released on impact it all comes out where the bullet hits
    // (in one cell, next to the wall); released at 1% a cell it is spread along the path (a
    // hundred cells would empty it; the wall is 120 cells away, so the rest comes out at the
    // impact). Either way, all of its mass comes out.
    elements_init();
    float mass = gas_mass_at(ELEM_HYDROGEN, 300e3f, g_elem_defs[ELEM_HYDROGEN].spawn_temp);
    for (int gradual = 0; gradual < 2; gradual++) {
        int pl;
        World *w = range(170 + (uint64_t)gradual, &pl);
        double before = test_mass(w, ELEM_HYDROGEN);
        BulletDesign d;
        bullet_design_clear(&d);
        put(&d, 0, BULLET_MATTER, ELEM_HYDROGEN, gradual ? RELEASE_GRADUAL : RELEASE_IMPACT, 300.0f,
            1.0f, 0);
        shoot(w, pl, &d, 2);
        // (All but the few percent of the trail that burns in the hot muzzle gas.)
        double burnt = (double)mass - (test_mass(w, ELEM_HYDROGEN) - before);
        EXPECT(burnt > -0.001 * (double)mass && burnt < (gradual ? 0.05 : 0.001) * (double)mass);
        int cells = 0;
        float biggest = 0.0f;
        for (int x = 1; x < 160; x++) {
            for (int y = 100; y < 127; y++) {
                const GasMix *g = &w->gas[world_index(w, x, y)];
                if (g->m[GAS_H2] > 1e-12f) {
                    cells++;
                    biggest = fmaxf(biggest, g->m[GAS_H2]);
                }
            }
        }
        if (gradual) {
            EXPECT(cells > 60);                        // along the path
            EXPECT(biggest < 0.5f * mass);
        } else {
            EXPECT(cells < 12);                        // at the impact (and what it spread into)
            EXPECT(biggest > 0.3f * mass);
        }
        EXPECT_EQ(test_count(w, ELEM_BULLET), 0); // no solid pixel: nothing left of the bullet
        world_destroy(w);
    }
}

void test_bullet_liquid_and_heat(void) {
    // A water pixel comes out as a drop at the impact; a heat pixel of 2000 degC heats the cells
    // it hits; and a bullet with a solid pixel goes on as a bullet (it leaves its pixel).
    int pl;
    World *w = range(172, &pl);
    BulletDesign d;
    bullet_design_clear(&d);
    put(&d, 0, BULLET_MATTER, ELEM_WATER, RELEASE_IMPACT, 0, 0, 0);
    put(&d, 1, BULLET_HEAT, ELEM_VOID, RELEASE_IMPACT, 0, 0, 2273.15f);
    shoot(w, pl, &d, 1);
    EXPECT_EQ(test_count(w, ELEM_WATER), 1);
    float hottest = 0.0f;
    for (int y = 1; y < 127; y++) {
        for (int x = 156; x < 163; x++) {
            hottest = fmaxf(hottest, w->temp[world_index(w, x, y)]);
        }
    }
    EXPECT(hottest > 2200.0f);
    world_destroy(w);
    w = range(173, &pl);
    bullet_design_clear(&d);
    put(&d, 0, BULLET_MATTER, ELEM_IRON, 0, 0, 0, 0);
    put(&d, 1, BULLET_MATTER, ELEM_OXYGEN, RELEASE_IMPACT, 100.0f, 0, 0);
    shoot(w, pl, &d, 60);
    EXPECT_EQ(test_count(w, ELEM_BULLET), 1); // the iron pixel went on as a bullet and landed
    world_destroy(w);
}

void test_example_bomb_bullet_burns(void) {
    // The example: a pixel of hydrogen and one of oxygen, 300 kPa each, released at 1% a cell, and
    // 2000 degC released on impact. Fired at a wall 120 cells away, the trail is spent, but at a
    // short range most of the gas is left for the impact: it burns, making steam.
    BulletDesign d;
    bullet_design_clear(&d);
    put(&d, 0, BULLET_MATTER, ELEM_HYDROGEN, RELEASE_GRADUAL, 300.0f, 1.0f, 0);
    put(&d, 1, BULLET_MATTER, ELEM_OXYGEN, RELEASE_GRADUAL, 300.0f, 1.0f, 0);
    put(&d, 2, BULLET_HEAT, ELEM_VOID, RELEASE_IMPACT, 0, 0, 2273.15f);
    int pl;
    World *w = range(174, &pl);
    // A wall 25 cells in front of the muzzle.
    for (int y = 1; y <= 126; y++) {
        Edit wall = {70, y, 80, y, 0, EDIT_SQUARE, ELEM_ROCK, 0, EDIT_PAINT, 0.0f};
        sim_apply_edit(w, &wall);
    }
    double h2 = 0.0;
    double steam_before = test_mass(w, ELEM_STEAM);
    shoot(w, pl, &d, 1);
    h2 = test_mass(w, ELEM_HYDROGEN);
    EXPECT(h2 > 1e-5);
    sim_run_ticks(w, 30);
    EXPECT(test_mass(w, ELEM_HYDROGEN) < 0.2 * h2);    // burnt
    EXPECT(test_mass(w, ELEM_STEAM) > steam_before + 1e-6); // into steam
    world_destroy(w);
}

void test_bullet_damage_comes_from_solids(void) {
    // Two iron pixels hit a walker for twice the damage of one; a bullet of only gas hurts no one.
    int pl;
    World *w = range(175, &pl);
    int mi = enemy_spawn(w, KIND_WALKER, 120, 126 - WALKER_H + 1);
    EXPECT(mi >= 0);
    Creature *m = &w->creatures[mi];
    BulletDesign d;
    bullet_design_clear(&d);
    put(&d, 0, BULLET_MATTER, ELEM_IRON, 0, 0, 0, 0);
    put(&d, 1, BULLET_MATTER, ELEM_IRON, 0, 0, 0, 0);
    Rifle r = {0};
    r.nslots = 1;
    r.slots[0] = d;
    float sx, sy;
    rifle_shoulder(&w->creatures[pl], &sx, &sy);
    EXPECT(rifle_update(w, &r, pl, (float)m->x + 3.0f, (float)m->y + 3.0f, true));
    sim_run_ticks(w, 1);
    EXPECT(fabsf(m->hp - (WALKER_HP - 2.0f * BULLET_SOLID_DAMAGE)) < 1e-3f);
    float hp = m->hp;
    bullet_design_clear(&d);
    put(&d, 0, BULLET_MATTER, ELEM_OXYGEN, RELEASE_IMPACT, 100.0f, 0, 0);
    r.slots[0] = d;
    r.cooldown = 0;
    EXPECT(rifle_update(w, &r, pl, (float)m->x + 3.0f, (float)m->y + 3.0f, true));
    sim_run_ticks(w, 1);
    EXPECT(m->hp == hp);
    world_destroy(w);
}
