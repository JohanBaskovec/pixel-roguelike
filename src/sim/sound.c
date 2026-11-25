#include "sim/sound.h"

#include "sim/world.h"

#include <string.h>

void world_sound(World *w, SoundKind kind, int x, int y, float volume) {
    if (w->sound_count >= SOUND_QUEUE_MAX) {
        return;
    }
    float v = PR_CLAMP(volume, 0.0f, 1.0f);
    w->sounds[w->sound_count++] = (SoundEvent){x, y, (uint8_t)kind, (uint8_t)(v * 255.0f + 0.5f)};
}

int world_sounds_drain(World *w, SoundEvent *out, int max) {
    int n = PR_MIN(w->sound_count, max);
    memcpy(out, w->sounds, (size_t)n * sizeof(SoundEvent));
    w->sound_count = 0;
    return n;
}
