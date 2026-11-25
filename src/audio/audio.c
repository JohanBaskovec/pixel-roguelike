#include "audio/audio.h"

#include "core/log.h"
#include "sim/world.h"

#include <SDL3/SDL_init.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI 6.28318530718f

// ---- Synthesis (start-up only; not the hot path) ----

static float noise_unit(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) / (float)(1u << 23) - 1.0f; // -1 .. 1
}

static float envelope(float t, float tau) {
    float attack = t < 0.002f ? t / 0.002f : 1.0f;
    return attack * expf(-t / tau);
}

// Noise from t0 for dur seconds through a one-pole low-pass whose coefficient goes a0 -> a1
// (1 = bright, small = dull), with an exponential decay.
static void add_noise(float *b, int n, float t0, float dur, float amp, float tau, float a0,
                      float a1, uint32_t *rng) {
    int i0 = (int)(t0 * AUDIO_RATE), cnt = (int)(dur * AUDIO_RATE);
    float lp = 0.0f;
    for (int i = 0; i < cnt && i0 + i < n; i++) {
        float t = (float)i / AUDIO_RATE, f = (float)i / (float)cnt;
        float a = a0 + (a1 - a0) * f;
        lp += a * (noise_unit(rng) - lp);
        b[i0 + i] += amp * envelope(t, tau) * lp * (1.0f + 0.8f * (1.0f - a));
    }
}

// A sine from t0 for dur seconds, its frequency sweeping f0 -> f1 (exponentially) with optional
// vibrato (rate Hz, depth as a fraction of the frequency).
static void add_sine(float *b, int n, float t0, float dur, float f0, float f1, float amp,
                     float tau, float vib_hz, float vib_depth) {
    int i0 = (int)(t0 * AUDIO_RATE), cnt = (int)(dur * AUDIO_RATE);
    float ph = 0.0f;
    for (int i = 0; i < cnt && i0 + i < n; i++) {
        float t = (float)i / AUDIO_RATE, f = (float)i / (float)cnt;
        float hz = f0 * powf(f1 / f0, f) * (1.0f + vib_depth * sinf(TWO_PI * vib_hz * t));
        ph += TWO_PI * hz / AUDIO_RATE;
        b[i0 + i] += amp * envelope(t, tau) * sinf(ph);
    }
}

static bool make_clip(Clip *c, float secs) {
    c->n = (int)(secs * AUDIO_RATE);
    c->s = calloc((size_t)c->n, sizeof(float));
    return c->s != NULL;
}

static void normalise(Clip *c, float peak) {
    float m = 1e-6f;
    for (int i = 0; i < c->n; i++) {
        m = fmaxf(m, fabsf(c->s[i]));
    }
    for (int i = 0; i < c->n; i++) {
        c->s[i] *= peak / m;
    }
    // A short fade-out so no clip ends in a click.
    int f = PR_MIN(c->n, 200);
    for (int i = 0; i < f; i++) {
        c->s[c->n - 1 - i] *= (float)i / (float)f;
    }
}

static bool synth(Clip *c, SoundKind k) {
    uint32_t r = 0x9E3779B9u * (uint32_t)(k + 1);
    float len = 0.5f;
    float peak = 0.8f;
    switch (k) {
        case SOUND_SHOT: len = 0.5f; peak = 1.0f; break;
        case SOUND_RELOAD: len = 0.4f; peak = 0.7f; break;
        case SOUND_IMPACT_DIRT: len = 0.2f; peak = 0.7f; break;
        case SOUND_IMPACT_HARD: len = 0.4f; peak = 0.8f; break;
        case SOUND_IMPACT_WOOD: len = 0.2f; peak = 0.75f; break;
        case SOUND_IMPACT_FLESH: len = 0.25f; peak = 0.8f; break;
        case SOUND_SPLASH: len = 0.5f; peak = 0.7f; break;
        case SOUND_BLAST: len = 1.3f; peak = 1.0f; break;
        case SOUND_BITE: len = 0.25f; peak = 0.8f; break;
        case SOUND_MONSTER_DIE: len = 0.65f; peak = 0.85f; break;
        case SOUND_PLAYER_HURT: len = 0.3f; peak = 0.85f; break;
        case SOUND_PLAYER_DIE: len = 1.1f; peak = 0.9f; break;
        case SOUND_PICKUP: len = 0.35f; peak = 0.6f; break;
        case SOUND_JUMP: len = 0.2f; peak = 0.5f; break;
        case SOUND_SPAWN: len = 0.4f; peak = 0.7f; break;
        case SOUND_SPLIT: len = 0.45f; peak = 0.85f; break;
        case SOUND_CLICK: len = 0.06f; peak = 0.5f; break;
        default: break;
    }
    if (!make_clip(c, len)) {
        return false;
    }
    float *b = c->s;
    int n = c->n;
    switch (k) {
        case SOUND_SHOT:
            add_noise(b, n, 0.0f, 0.5f, 0.9f, 0.07f, 0.6f, 0.1f, &r);
            add_sine(b, n, 0.0f, 0.25f, 150.0f, 45.0f, 0.8f, 0.08f, 0, 0);
            add_noise(b, n, 0.0f, 0.03f, 0.7f, 0.01f, 0.95f, 0.9f, &r);
            break;
        case SOUND_RELOAD:
            add_noise(b, n, 0.0f, 0.03f, 0.6f, 0.01f, 0.8f, 0.8f, &r);
            add_sine(b, n, 0.0f, 0.1f, 1500.0f, 1500.0f, 0.15f, 0.02f, 0, 0);
            add_noise(b, n, 0.18f, 0.04f, 0.8f, 0.012f, 0.7f, 0.5f, &r);
            add_sine(b, n, 0.18f, 0.15f, 900.0f, 700.0f, 0.2f, 0.03f, 0, 0);
            break;
        case SOUND_IMPACT_DIRT:
            add_noise(b, n, 0.0f, 0.2f, 0.8f, 0.04f, 0.25f, 0.1f, &r);
            break;
        case SOUND_IMPACT_HARD:
            add_noise(b, n, 0.0f, 0.02f, 0.5f, 0.006f, 0.9f, 0.9f, &r);
            add_sine(b, n, 0.0f, 0.4f, 2200.0f, 2200.0f, 0.3f, 0.08f, 0, 0);
            add_sine(b, n, 0.0f, 0.3f, 3300.0f, 3300.0f, 0.2f, 0.05f, 0, 0);
            add_sine(b, n, 0.0f, 0.4f, 1400.0f, 1400.0f, 0.2f, 0.1f, 0, 0);
            break;
        case SOUND_IMPACT_WOOD:
            add_sine(b, n, 0.0f, 0.2f, 260.0f, 200.0f, 0.6f, 0.03f, 0, 0);
            add_noise(b, n, 0.0f, 0.04f, 0.5f, 0.012f, 0.5f, 0.3f, &r);
            break;
        case SOUND_IMPACT_FLESH:
            add_noise(b, n, 0.0f, 0.25f, 0.6f, 0.06f, 0.3f, 0.08f, &r);
            add_sine(b, n, 0.0f, 0.2f, 110.0f, 60.0f, 0.5f, 0.08f, 0, 0);
            break;
        case SOUND_SPLASH:
            add_noise(b, n, 0.0f, 0.5f, 0.6f, 0.12f, 0.5f, 0.15f, &r);
            add_noise(b, n, 0.05f, 0.3f, 0.4f, 0.08f, 0.35f, 0.1f, &r);
            break;
        case SOUND_BLAST:
            add_noise(b, n, 0.0f, 1.3f, 1.0f, 0.3f, 0.5f, 0.03f, &r);
            add_sine(b, n, 0.0f, 0.6f, 70.0f, 30.0f, 1.0f, 0.25f, 0, 0);
            add_noise(b, n, 0.0f, 0.04f, 0.8f, 0.012f, 0.95f, 0.9f, &r);
            break;
        case SOUND_BITE:
            add_noise(b, n, 0.0f, 0.07f, 0.6f, 0.03f, 0.5f, 0.3f, &r);
            add_noise(b, n, 0.09f, 0.08f, 0.6f, 0.03f, 0.4f, 0.2f, &r);
            add_sine(b, n, 0.0f, 0.2f, 180.0f, 100.0f, 0.3f, 0.06f, 0, 0);
            break;
        case SOUND_MONSTER_DIE:
            add_sine(b, n, 0.0f, 0.65f, 700.0f, 90.0f, 0.5f, 0.35f, 30.0f, 0.06f);
            add_noise(b, n, 0.0f, 0.3f, 0.3f, 0.1f, 0.5f, 0.2f, &r);
            break;
        case SOUND_PLAYER_HURT:
            add_sine(b, n, 0.0f, 0.3f, 260.0f, 170.0f, 0.6f, 0.12f, 0, 0);
            add_sine(b, n, 0.0f, 0.3f, 520.0f, 340.0f, 0.25f, 0.1f, 0, 0);
            add_noise(b, n, 0.0f, 0.04f, 0.3f, 0.015f, 0.6f, 0.4f, &r);
            break;
        case SOUND_PLAYER_DIE:
            add_sine(b, n, 0.0f, 1.1f, 300.0f, 60.0f, 0.6f, 0.5f, 6.0f, 0.04f);
            add_sine(b, n, 0.0f, 1.1f, 600.0f, 120.0f, 0.2f, 0.4f, 6.0f, 0.04f);
            add_noise(b, n, 0.0f, 0.4f, 0.3f, 0.15f, 0.3f, 0.1f, &r);
            break;
        case SOUND_PICKUP:
            add_sine(b, n, 0.0f, 0.12f, 660.0f, 660.0f, 0.4f, 0.1f, 0, 0);
            add_sine(b, n, 0.1f, 0.25f, 990.0f, 990.0f, 0.4f, 0.12f, 0, 0);
            break;
        case SOUND_JUMP:
            add_sine(b, n, 0.0f, 0.15f, 180.0f, 420.0f, 0.35f, 0.08f, 0, 0);
            add_noise(b, n, 0.0f, 0.15f, 0.15f, 0.06f, 0.1f, 0.1f, &r);
            break;
        case SOUND_SPAWN:
            add_sine(b, n, 0.0f, 0.35f, 300.0f, 900.0f, 0.35f, 0.25f, 14.0f, 0.05f);
            add_sine(b, n, 0.0f, 0.35f, 150.0f, 450.0f, 0.25f, 0.25f, 14.0f, 0.05f);
            break;
        case SOUND_SPLIT:
            add_noise(b, n, 0.0f, 0.12f, 0.8f, 0.03f, 0.7f, 0.3f, &r);
            add_sine(b, n, 0.0f, 0.15f, 200.0f, 90.0f, 0.5f, 0.05f, 0, 0);
            add_noise(b, n, 0.1f, 0.25f, 0.4f, 0.08f, 0.4f, 0.1f, &r);
            break;
        case SOUND_CLICK:
            add_noise(b, n, 0.0f, 0.04f, 0.5f, 0.008f, 0.8f, 0.8f, &r);
            add_sine(b, n, 0.0f, 0.05f, 1200.0f, 1200.0f, 0.2f, 0.01f, 0, 0);
            break;
        default:
            break;
    }
    normalise(c, peak);
    return true;
}

// The jetpack: a roar of dull noise with a brighter hiss, seamless as a loop (the end is
// cross-faded into the start).
static bool synth_jet(Clip *c) {
    const int fade = 4000;
    uint32_t r = 0x51ED270Bu;
    Clip raw;
    if (!make_clip(&raw, 1.0f + (float)fade / AUDIO_RATE)) {
        return false;
    }
    add_noise(raw.s, raw.n, 0.0f, 10.0f, 0.7f, 1e9f, 0.07f, 0.07f, &r);
    add_noise(raw.s, raw.n, 0.0f, 10.0f, 0.12f, 1e9f, 0.3f, 0.3f, &r);
    c->n = raw.n - fade;
    c->s = malloc((size_t)c->n * sizeof(float));
    if (!c->s) {
        free(raw.s);
        return false;
    }
    for (int i = 0; i < c->n; i++) {
        float v = raw.s[i];
        if (i < fade) { // blend in the samples that followed the loop's end
            float f = (float)i / (float)fade;
            v = v * f + raw.s[c->n + i] * (1.0f - f);
        }
        c->s[i] = v;
    }
    free(raw.s);
    normalise(c, 0.5f);
    return true;
}

// ---- Mixer ----

bool mixer_init(Mixer *m) {
    memset(m, 0, sizeof(*m));
    m->rng = 12345u;
    m->master = 0.6f;
    for (int k = 0; k < SOUND_KIND_COUNT; k++) {
        if (!synth(&m->clips[k], (SoundKind)k)) {
            mixer_free(m);
            return false;
        }
    }
    if (!synth_jet(&m->jet)) {
        mixer_free(m);
        return false;
    }
    return true;
}

void mixer_free(Mixer *m) {
    for (int k = 0; k < SOUND_KIND_COUNT; k++) {
        free(m->clips[k].s);
    }
    free(m->jet.s);
    memset(m, 0, sizeof(*m));
}

static void voice_remove(Mixer *m, int i) {
    memmove(&m->voices[i], &m->voices[i + 1], (size_t)(m->nvoices - i - 1) * sizeof(Voice));
    m->nvoices--;
}

void mixer_play(Mixer *m, SoundKind kind, float gain, float pan) {
    if (kind >= SOUND_KIND_COUNT || gain <= 0.0f || !m->clips[kind].s) {
        return;
    }
    int same = 0, oldest_same = -1;
    for (int i = 0; i < m->nvoices; i++) {
        if (m->voices[i].kind == kind) {
            same++;
            if (oldest_same < 0) {
                oldest_same = i;
            }
        }
    }
    if (same >= MIXER_PER_KIND) {
        voice_remove(m, oldest_same);
    }
    if (m->nvoices >= MIXER_VOICES) {
        voice_remove(m, 0);
    }
    pan = PR_CLAMP(pan, -1.0f, 1.0f);
    float rate = 0.9f + 0.2f * (noise_unit(&m->rng) * 0.5f + 0.5f); // a little pitch variation
    m->voices[m->nvoices++] = (Voice){&m->clips[kind], 0.0f, rate,
                                      gain * sqrtf(1.0f - pan), gain * sqrtf(1.0f + pan),
                                      (uint8_t)kind};
}

void mixer_event(Mixer *m, const SoundEvent *e, float lx, float ly) {
    float dx = (float)e->x - lx, dy = (float)e->y - ly;
    float d = sqrtf(dx * dx + dy * dy);
    if (d > MIXER_HEARING) {
        return;
    }
    float g = ((float)e->volume / 255.0f) / (1.0f + (d / 120.0f) * (d / 120.0f));
    if (g < 0.01f) {
        return;
    }
    mixer_play(m, (SoundKind)e->kind, g, PR_CLAMP(dx / 250.0f, -1.0f, 1.0f) * 0.8f);
}

void mixer_set_jet(Mixer *m, float level) {
    m->jet_target = PR_CLAMP(level, 0.0f, 1.0f);
}

void mixer_render(Mixer *m, float *out, int frames) {
    for (int v = 0; v < m->nvoices;) {
        Voice *vo = &m->voices[v];
        const float *s = vo->clip->s;
        int n = vo->clip->n;
        int i = 0;
        for (; i < frames && vo->pos < (float)(n - 1); i++) {
            int p = (int)vo->pos;
            float f = vo->pos - (float)p;
            float x = s[p] + (s[p + 1] - s[p]) * f;
            out[2 * i] += x * vo->gl * m->master;
            out[2 * i + 1] += x * vo->gr * m->master;
            vo->pos += vo->rate;
        }
        if (vo->pos >= (float)(n - 1)) {
            voice_remove(m, v);
        } else {
            v++;
        }
    }
    if (m->jet_level > 0.001f || m->jet_target > 0.0f) {
        for (int i = 0; i < frames; i++) {
            m->jet_level += (m->jet_target - m->jet_level) * 0.0004f;
            int p = (int)m->jet_pos;
            float x = m->jet.s[p] * m->jet_level * 0.5f * m->master;
            out[2 * i] += x;
            out[2 * i + 1] += x;
            if (++m->jet_pos >= (float)m->jet.n) {
                m->jet_pos = 0.0f;
            }
        }
    }
    for (int i = 0; i < 2 * frames; i++) {
        out[i] = tanhf(out[i]); // a soft limit: many sounds at once do not clip
    }
}

// ---- The device ----

#define DEVICE_AHEAD_FRAMES 3000 // about 70 ms of audio is kept queued
#define BLOCK_FRAMES        512

bool audio_init(Audio *a) {
    memset(a, 0, sizeof(*a));
    if (!mixer_init(&a->mixer)) {
        pr_log_error("audio: could not build the sounds");
        return false;
    }
    a->ready = true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        pr_log_error("audio: no audio (%s); the game is silent", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, AUDIO_RATE};
    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!a->stream) {
        pr_log_error("audio: no playback device (%s); the game is silent", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(a->stream);
    return true;
}

void audio_shutdown(Audio *a) {
    if (a->stream) {
        SDL_DestroyAudioStream(a->stream);
    }
    if (a->ready) {
        mixer_free(&a->mixer);
    }
    memset(a, 0, sizeof(*a));
}

void audio_play_ui(Audio *a, SoundKind kind) {
    if (a->ready && !a->muted) {
        mixer_play(&a->mixer, kind, 0.8f, 0.0f);
    }
}

void audio_frame(Audio *a, World *w, float lx, float ly, float jet) {
    SoundEvent ev[SOUND_QUEUE_MAX];
    int n = world_sounds_drain(w, ev, SOUND_QUEUE_MAX);
    if (!a->ready) {
        return;
    }
    if (!a->muted) {
        for (int i = 0; i < n; i++) {
            mixer_event(&a->mixer, &ev[i], lx, ly);
        }
    }
    mixer_set_jet(&a->mixer, a->muted ? 0.0f : jet);
    if (!a->stream) {
        return;
    }
    int want = DEVICE_AHEAD_FRAMES * 2 * (int)sizeof(float);
    for (int blocks = 0; blocks < 16 && SDL_GetAudioStreamQueued(a->stream) < want; blocks++) {
        float buf[2 * BLOCK_FRAMES] = {0};
        mixer_render(&a->mixer, buf, BLOCK_FRAMES);
        SDL_PutAudioStreamData(a->stream, buf, (int)sizeof(buf));
    }
}
