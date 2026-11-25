#pragma once

#include "core/common.h"

// Sound events (D-049). The simulation is headless: where something makes a noise it queues an
// event (what, where, how loud) in the world, and the game drains the queue each frame and plays
// it (audio/audio.c). Events are queued only from serial code (never inside a parallel pass), and
// the queue drops the newest ones when it is full.
typedef enum SoundKind {
    SOUND_SHOT,
    SOUND_RELOAD,
    SOUND_IMPACT_DIRT,  // a bullet in dirt, sand and other powders
    SOUND_IMPACT_HARD,  // rock, iron: a ping
    SOUND_IMPACT_WOOD,
    SOUND_IMPACT_FLESH, // a bullet in a monster
    SOUND_SPLASH,       // a bullet into a liquid
    SOUND_BLAST,        // a drawn bullet's gas and heat released
    SOUND_BITE,
    SOUND_MONSTER_DIE,
    SOUND_PLAYER_HURT,
    SOUND_PLAYER_DIE,
    SOUND_PICKUP,
    SOUND_JUMP,
    SOUND_SPAWN,        // a monster or a log placed by the player
    SOUND_SPLIT,        // a rigid body cut in pieces
    SOUND_CLICK,        // user interface
    SOUND_KIND_COUNT
} SoundKind;

typedef struct SoundEvent {
    int32_t x, y;  // cell
    uint8_t kind;  // SoundKind
    uint8_t volume; // 0..255, 255 = full
} SoundEvent;

#define SOUND_QUEUE_MAX 256

struct World;

// Queues an event. volume is 0..1 (clamped).
void world_sound(struct World *w, SoundKind kind, int x, int y, float volume);

// Copies out up to `max` queued events and empties the queue. Returns how many.
int world_sounds_drain(struct World *w, SoundEvent *out, int max);
