#include "test.h"

#include "audio/audio.h"
#include "sim/creature.h"
#include "sim/edit.h"
#include "sim/enemy.h"
#include "sim/pickup.h"
#include "sim/scene.h"
#include "sim/sim.h"
#include "sim/weapon.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void test_world_queues_sounds(void);
void test_events_for_shots_hits_and_deaths(void);
void test_mixer_plays_and_limits(void);

static void fill(World *w, int x0, int y0, int x1, int y1, ElementId e) {
    for (int y = y0; y <= y1; y++) {
        Edit ed = {x0, y, x1, y, 0, EDIT_SQUARE, e, 0, EDIT_PAINT, 0.0f};
        sim_apply_edit(w, &ed);
    }
}

static int count_kind(const SoundEvent *ev, int n, SoundKind k) {
    int c = 0;
    for (int i = 0; i < n; i++) {
        c += ev[i].kind == k;
    }
    return c;
}

void test_world_queues_sounds(void) {
    World *w = test_world_box(64, 64, 1);
    world_sound(w, SOUND_SHOT, 10, 20, 1.0f);
    world_sound(w, SOUND_CLICK, 11, 21, 7.0f); // clamped
    SoundEvent ev[SOUND_QUEUE_MAX];
    int n = world_sounds_drain(w, ev, SOUND_QUEUE_MAX);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(ev[0].kind, SOUND_SHOT);
    EXPECT_EQ(ev[0].x, 10);
    EXPECT_EQ(ev[1].volume, 255);
    EXPECT_EQ(world_sounds_drain(w, ev, SOUND_QUEUE_MAX), 0);
    for (int i = 0; i < SOUND_QUEUE_MAX + 50; i++) { // a full queue drops, never overflows
        world_sound(w, SOUND_CLICK, 0, 0, 1.0f);
    }
    EXPECT_EQ(world_sounds_drain(w, ev, SOUND_QUEUE_MAX), SOUND_QUEUE_MAX);
    world_destroy(w);
}

// A room with the player at the left and a wall at the right: a shot sounds, and its impact.
// A monster that is shot sounds, and its death; a crate that is picked up sounds.
void test_events_for_shots_hits_and_deaths(void) {
    World *w = test_world_box(192, 128, 3);
    fill(w, 1, 1, 190, 126, EDIT_AIR);
    fill(w, 160, 1, 190, 126, ELEM_IRON);
    int player = scene_spawn_player(w, 60);
    sim_run_ticks(w, 30);
    SoundEvent ev[SOUND_QUEUE_MAX];
    world_sounds_drain(w, ev, SOUND_QUEUE_MAX);

    Rifle r = {0};
    float sx, sy;
    rifle_shoulder(&w->creatures[player], &sx, &sy);
    rifle_update(w, &r, player, sx + 100.0f, sy, true);
    for (int i = 0; i < 20; i++) {
        sim_tick(w, NULL);
    }
    int n = world_sounds_drain(w, ev, SOUND_QUEUE_MAX);
    EXPECT_EQ(count_kind(ev, n, SOUND_SHOT), 1);
    EXPECT_EQ(count_kind(ev, n, SOUND_IMPACT_HARD), 1); // once per bullet, whatever it hits

    // A monster in the line of fire.
    int m = enemy_spawn(w, KIND_WALKER, 120, 100);
    EXPECT(m >= 0);
    Creature *c = &w->creatures[m];
    creature_hit(w, c, 1.0f, c->x, c->y, 1.0f, 0.0f, c->x, c->y);
    n = world_sounds_drain(w, ev, SOUND_QUEUE_MAX);
    EXPECT_EQ(count_kind(ev, n, SOUND_IMPACT_FLESH), 1);
    creature_hit(w, c, 1e6f, c->x, c->y, 1.0f, 0.0f, c->x, c->y);
    n = world_sounds_drain(w, ev, SOUND_QUEUE_MAX);
    EXPECT_EQ(count_kind(ev, n, SOUND_MONSTER_DIE), 1);

    // A health crate under a hurt player.
    w->creatures[player].hp = 10.0f;
    Creature *p = &w->creatures[player];
    pickup_add(w, PICKUP_HEALTH, p->x + p->w / 2, p->y + p->h - 1);
    pickups_collect(w, player);
    n = world_sounds_drain(w, ev, SOUND_QUEUE_MAX);
    EXPECT_EQ(count_kind(ev, n, SOUND_PICKUP), 1);
    world_destroy(w);
}

static float peak(const float *b, int n) {
    float m = 0.0f;
    for (int i = 0; i < n; i++) {
        m = fmaxf(m, fabsf(b[i]));
    }
    return m;
}

void test_mixer_plays_and_limits(void) {
    static Mixer m;
    EXPECT(mixer_init(&m));
    static float out[2 * 4096];
    mixer_render(&m, out, 4096);
    EXPECT(peak(out, 2 * 4096) == 0.0f); // nothing playing: silence

    // Every kind makes a sound that ends (all clips are within 1.5 s).
    for (int k = 0; k < SOUND_KIND_COUNT; k++) {
        mixer_play(&m, (SoundKind)k, 1.0f, 0.0f);
        memset(out, 0, sizeof(out));
        mixer_render(&m, out, 4096);
        EXPECT(peak(out, 2 * 4096) > 0.01f);
        for (int i = 0; i < 20; i++) {
            memset(out, 0, sizeof(out));
            mixer_render(&m, out, 4096);
        }
        EXPECT_EQ(m.nvoices, 0);
    }

    // Many at once: capped per kind and overall, and never louder than 1 (soft limit).
    for (int i = 0; i < 100; i++) {
        mixer_play(&m, SOUND_SHOT, 1.0f, 0.0f);
    }
    EXPECT(m.nvoices <= MIXER_PER_KIND);
    for (int k = 0; k < SOUND_KIND_COUNT; k++) {
        for (int i = 0; i < 10; i++) {
            mixer_play(&m, (SoundKind)k, 1.0f, 0.0f);
        }
    }
    EXPECT(m.nvoices <= MIXER_VOICES);
    memset(out, 0, sizeof(out));
    mixer_render(&m, out, 4096);
    EXPECT(peak(out, 2 * 4096) <= 1.0f);

    // Distance and side: far is quieter than near, nothing beyond hearing; a sound on the right
    // is louder in the right channel.
    mixer_free(&m);
    EXPECT(mixer_init(&m));
    SoundEvent near_e = {100, 100, SOUND_BLAST, 255}, far_e = {100 + 400, 100, SOUND_BLAST, 255};
    SoundEvent gone = {100 + 800, 100, SOUND_BLAST, 255}, right = {100 + 150, 100, SOUND_BLAST, 255};
    mixer_event(&m, &gone, 100, 100);
    EXPECT_EQ(m.nvoices, 0);
    mixer_event(&m, &near_e, 100, 100);
    float gn = m.voices[0].gl;
    mixer_event(&m, &far_e, 100, 100);
    EXPECT(m.voices[1].gl < gn * 0.2f);
    mixer_event(&m, &right, 100, 100);
    EXPECT(m.voices[2].gr > m.voices[2].gl);
    mixer_free(&m);
}
