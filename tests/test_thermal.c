#include "test.h"

#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/enemy.h"
#include "sim/sim.h"

void test_heat_burns_people_and_monsters(void);
void test_cold_freezes_people_and_monsters(void);
void test_comfortable_temperatures_do_not_hurt(void);
void test_monster_dies_of_heat(void);

static void fill_air(World *w) {
    for (int y = 1; y <= w->height - 2; y++) {
        Edit ed = {1, y, w->width - 2, y, 0, EDIT_SQUARE, EDIT_AIR, 0, EDIT_PAINT, 0.0f, 0.0f};
        sim_apply_edit(w, &ed);
    }
}

static void set_temperature(World *w, float k) {
    for (int y = 1; y <= w->height - 2; y++) {
        for (int x = 1; x <= w->width - 2; x++) {
            if (world_elem(w, x, y) != ELEM_BODY) { // the body starts at its own temperature
                w->temp[world_index(w, x, y)] = k;
            }
        }
    }
}

// A room of air at `k` kelvin with one creature standing in it (a person, or a monster kind).
static World *room(float k, int kind, int *id) {
    World *w = test_world_box(128, 128, 5);
    fill_air(w);
    *id = kind == KIND_PERSON ? creature_spawn(w, 60, 100) : enemy_spawn(w, kind, 60, 100);
    set_temperature(w, k); // the body itself stays at 37 degC (the air has to hurt it)
    sim_run_ticks(w, 5);
    return w;
}

void test_heat_burns_people_and_monsters(void) {
    int kinds[] = {KIND_PERSON, KIND_WALKER, KIND_FLYER};
    for (int i = 0; i < 3; i++) {
        int id;
        World *w = room(380.0f, kinds[i], &id); // 107 degC
        EXPECT(id >= 0);
        float hp0 = w->creatures[id].hp_max;
        sim_run_ticks(w, 30);
        const Creature *c = &w->creatures[id];
        EXPECT(c->hp < hp0 - 5.0f);
        EXPECT_EQ(c->thermal, 1);
        world_destroy(w);
    }
}

void test_cold_freezes_people_and_monsters(void) {
    int id;
    World *w = room(250.0f, KIND_WALKER, &id); // -23 degC
    float hp0 = w->creatures[id].hp_max;
    sim_run_ticks(w, 30);
    EXPECT(w->creatures[id].hp < hp0 - 3.0f);
    EXPECT_EQ(w->creatures[id].thermal, -1);
    world_destroy(w);
    // A person in the cold: body heat cannot hold the skin up in air this cold.
    w = room(200.0f, KIND_PERSON, &id);
    hp0 = w->creatures[id].hp_max;
    sim_run_ticks(w, 60);
    EXPECT(w->creatures[id].hp < hp0 - 3.0f);
    EXPECT_EQ(w->creatures[id].thermal, -1);
    world_destroy(w);
}

void test_comfortable_temperatures_do_not_hurt(void) {
    int kinds[] = {KIND_PERSON, KIND_WALKER, KIND_FLYER};
    for (int i = 0; i < 3; i++) {
        int id;
        World *w = room(T_ROOM, kinds[i], &id);
        float hp0 = w->creatures[id].hp;
        sim_run_ticks(w, 120);
        EXPECT(w->creatures[id].hp == hp0);
        EXPECT_EQ(w->creatures[id].thermal, 0);
        world_destroy(w);
    }
}

void test_monster_dies_of_heat(void) {
    int id;
    World *w = room(900.0f, KIND_WALKER, &id);
    sim_run_ticks(w, 240); // 160 hp at no more than 60 hp a second
    const Creature *c = &w->creatures[id];
    EXPECT_EQ(c->state, CREATURE_DEAD);
    EXPECT(c->corpse); // what is left is a rigid body
    world_destroy(w);
}
