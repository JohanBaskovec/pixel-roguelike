#pragma once

#include "core/common.h"
#include "sim/sound.h"

#include <SDL3/SDL_audio.h>

// Sound (D-049): every sound is synthesised at start-up from noise and sine sweeps (no assets, no
// dependency beyond SDL), then mixed by a small software mixer. The mixer is plain code with no
// device (testable); `Audio` adds an SDL playback stream that the game feeds once per frame.

#define AUDIO_RATE   44100
#define MIXER_VOICES 32
#define MIXER_PER_KIND 5 // one kind never sounds on more voices than this (a burst of impacts)

typedef struct Clip {
    float *s; // mono samples
    int n;
} Clip;

typedef struct Voice {
    const Clip *clip;
    float pos, rate; // sample position and playback rate (pitch variation)
    float gl, gr;    // gain left and right
    uint8_t kind;
} Voice;

typedef struct Mixer {
    Clip clips[SOUND_KIND_COUNT];
    Clip jet; // the jetpack's loop
    Voice voices[MIXER_VOICES];
    int nvoices; // oldest first
    float jet_pos, jet_level, jet_target;
    uint32_t rng;
    float master;
} Mixer;

bool mixer_init(Mixer *m);
void mixer_free(Mixer *m);

// Starts a sound with a gain (0..1) and a pan (-1 left .. 1 right).
void mixer_play(Mixer *m, SoundKind kind, float gain, float pan);

// Starts the sound of a world event as heard by a listener at cell (lx, ly): quieter with
// distance (nothing beyond MIXER_HEARING cells), panned by the side it comes from.
#define MIXER_HEARING 700.0f
void mixer_event(Mixer *m, const SoundEvent *e, float lx, float ly);

// The jetpack's loop: 0 off .. 1 full. It fades in and out.
void mixer_set_jet(Mixer *m, float level);

// Adds `frames` stereo frames (interleaved) to `out`, which must be zeroed or hold what to add to.
void mixer_render(Mixer *m, float *out, int frames);

typedef struct Audio {
    Mixer mixer;
    SDL_AudioStream *stream; // NULL: no device (the game is silent)
    bool muted;
    bool ready; // the mixer is initialised
} Audio;

// Opens the default playback device. Without one, the game runs silent (returns false, but
// the mixer works: audio_ok() is false).
bool audio_init(Audio *a);
void audio_shutdown(Audio *a);

// A sound that is not in the world (the interface): centred, full distance.
void audio_play_ui(Audio *a, SoundKind kind);

// Once a frame: plays the world's queued sound events as heard from (lx, ly), sets the jetpack
// loop (0..1), and keeps the device fed.
struct World;
void audio_frame(Audio *a, struct World *w, float lx, float ly, float jet);
