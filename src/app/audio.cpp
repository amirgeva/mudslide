#include "app/audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mud {
namespace {

constexpr float kTwoPi = 6.28318530718f;

struct Noise {
    uint32_t s = 0x12345678u;
    float next() {
        s = s * 1664525u + 1013904223u;
        return float(s >> 8) / float(1u << 23) - 1.f;
    }
};

// One-pole low-pass filter.
struct LowPass {
    float y = 0.f;
    float operator()(float x, float k) { return y += (x - y) * k; }
};

}  // namespace

bool Audio::init() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_Log("Audio unavailable: %s", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = &Audio::callback;
    want.userdata = this;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!device_) {
        SDL_Log("No audio device: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    rate_ = have.freq;
    synthesize();
    voices_.reserve(32);
    SDL_PauseAudioDevice(device_, 0);
    return true;
}

void Audio::shutdown() {
    if (device_) {
        SDL_CloseAudioDevice(device_);
        device_ = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void Audio::play(Sfx sfx, float volume) {
    if (!device_ || muted_) return;
    SDL_LockAudioDevice(device_);
    if (voices_.size() < 24) voices_.push_back(Voice{int(sfx), 0, volume});
    SDL_UnlockAudioDevice(device_);
}

void SDLCALL Audio::callback(void* user, Uint8* stream, int len) {
    auto* self = static_cast<Audio*>(user);
    float* out = reinterpret_cast<float*>(stream);
    const size_t frames = size_t(len) / sizeof(float);
    std::fill(out, out + frames, 0.f);
    for (size_t v = 0; v < self->voices_.size();) {
        Voice& voice = self->voices_[v];
        const std::vector<float>& snd = self->sounds_[voice.sound];
        const size_t n = std::min(frames, snd.size() - voice.pos);
        for (size_t i = 0; i < n; ++i) out[i] += snd[voice.pos + i] * voice.volume;
        voice.pos += n;
        if (voice.pos >= snd.size()) {
            self->voices_[v] = self->voices_.back();
            self->voices_.pop_back();
        } else {
            ++v;
        }
    }
    // Soft clip so stacked explosions saturate instead of crackling.
    for (size_t i = 0; i < frames; ++i) out[i] = std::tanh(out[i] * 1.2f) * 0.85f;
}

void Audio::synthesize() {
    const float rate = float(rate_);
    auto make = [&](float seconds) { return std::vector<float>(size_t(seconds * rate)); };
    Noise noise;

    {  // Fire: a deep cannon thump with a burst of noise.
        auto& s = sounds_[int(Sfx::Fire)] = make(0.5f);
        float phase = 0.f;
        LowPass lp;
        for (size_t i = 0; i < s.size(); ++i) {
            const float t = float(i) / rate;
            phase += kTwoPi * (150.f * std::exp(-t * 9.f) + 45.f) / rate;
            const float body = std::sin(phase) * std::exp(-t * 8.f);
            const float burst = lp(noise.next(), 0.35f * std::exp(-t * 12.f) + 0.02f) * std::exp(-t * 18.f);
            s[i] = (body * 0.7f + burst * 1.4f) * std::min(1.f, t * 800.f) * 0.55f;
        }
    }
    auto boom = [&](std::vector<float>& s, float decay, float cutoff, float rumbleHz, float gain, bool crackle) {
        LowPass lp, lp2;
        float phase = 0.f;
        for (size_t i = 0; i < s.size(); ++i) {
            const float t = float(i) / rate;
            const float k = cutoff * std::exp(-t * 3.f) + 0.01f;
            float v = lp2(lp(noise.next(), k), k) * 2.2f;
            phase += kTwoPi * rumbleHz * (1.f - 0.4f * std::min(1.f, t * 2.f)) / rate;
            v += std::sin(phase) * 0.6f * std::exp(-t * 6.f);
            if (crackle && noise.next() > 0.996f - 0.004f * std::exp(-t * 2.f)) v += noise.next() * 0.8f * std::exp(-t * 1.5f);
            s[i] = v * std::exp(-t * decay) * std::min(1.f, t * 600.f) * gain;
        }
    };
    boom(sounds_[int(Sfx::Boom)] = make(0.9f), 5.f, 0.25f, 55.f, 0.75f, false);
    boom(sounds_[int(Sfx::BigBoom)] = make(1.7f), 2.6f, 0.18f, 42.f, 0.95f, true);
    boom(sounds_[int(Sfx::Crack)] = make(0.4f), 11.f, 0.45f, 90.f, 0.45f, false);

    {  // Split: a sharp pop as a cluster bursts.
        auto& s = sounds_[int(Sfx::Split)] = make(0.14f);
        float phase = 0.f;
        for (size_t i = 0; i < s.size(); ++i) {
            const float t = float(i) / rate;
            phase += kTwoPi * (1100.f - 5000.f * t) / rate;
            s[i] = (std::sin(phase) * 0.6f + noise.next() * 0.5f * std::exp(-t * 80.f)) * std::exp(-t * 30.f) * 0.3f;
        }
    }
    {  // Splat: a wet slap with a gloopy downward bloop.
        auto& s = sounds_[int(Sfx::Splat)] = make(0.6f);
        LowPass lp;
        float phase = 0.f;
        for (size_t i = 0; i < s.size(); ++i) {
            const float t = float(i) / rate;
            const float slap = lp(noise.next(), 0.12f + 0.3f * std::exp(-t * 20.f)) * 3.f;
            const float am = 0.6f + 0.4f * std::sin(kTwoPi * 28.f * t);
            phase += kTwoPi * (320.f * std::exp(-t * 6.f) + 70.f) / rate;
            const float bloop = std::sin(phase) * std::exp(-t * 7.f);
            s[i] = (slap * am * std::exp(-t * 9.f) + bloop * 0.5f) * std::min(1.f, t * 500.f) * 0.55f;
        }
    }
    {  // Thud: a tank hitting the ground.
        auto& s = sounds_[int(Sfx::Thud)] = make(0.25f);
        float phase = 0.f;
        for (size_t i = 0; i < s.size(); ++i) {
            const float t = float(i) / rate;
            phase += kTwoPi * (95.f - 120.f * t) / rate;
            s[i] = (std::sin(phase) + noise.next() * 0.3f * std::exp(-t * 60.f)) * std::exp(-t * 18.f) * 0.5f;
        }
    }
    {  // Bore: grinding through dirt.
        auto& s = sounds_[int(Sfx::Bore)] = make(0.45f);
        LowPass lp;
        for (size_t i = 0; i < s.size(); ++i) {
            const float t = float(i) / rate;
            const float grit = lp(noise.next(), 0.2f) * (0.5f + 0.5f * std::sin(kTwoPi * 55.f * t));
            s[i] = grit * std::exp(-t * 6.f) * std::min(1.f, t * 300.f) * 1.2f;
        }
    }
    {  // Tick: menu navigation.
        auto& s = sounds_[int(Sfx::Tick)] = make(0.035f);
        for (size_t i = 0; i < s.size(); ++i) {
            const float t = float(i) / rate;
            s[i] = std::sin(kTwoPi * 1250.f * t) * std::exp(-t * 140.f) * 0.10f;
        }
    }
    {  // Chime: your turn.
        auto& s = sounds_[int(Sfx::Chime)] = make(0.6f);
        const float notes[2] = {659.25f, 987.77f};
        for (int n = 0; n < 2; ++n) {
            const float start = 0.08f * float(n);
            for (size_t i = size_t(start * rate); i < s.size(); ++i) {
                const float t = float(i) / rate - start;
                const float ph = kTwoPi * notes[n] * t;
                s[i] += (std::sin(ph) + 0.25f * std::sin(2.f * ph)) * std::exp(-t * 6.f) * std::min(1.f, t * 400.f) * 0.09f;
            }
        }
    }
    {  // Fanfare: a bright rising arpeggio for the winner.
        auto& s = sounds_[int(Sfx::Fanfare)] = make(1.4f);
        const float notes[6] = {392.f, 523.25f, 659.25f, 783.99f, 659.25f, 1046.5f};
        const float starts[6] = {0.f, 0.11f, 0.22f, 0.33f, 0.50f, 0.58f};
        for (int n = 0; n < 6; ++n) {
            for (size_t i = size_t(starts[n] * rate); i < s.size(); ++i) {
                const float t = float(i) / rate - starts[n];
                const float env = std::exp(-t * (n == 5 ? 2.5f : 7.f)) * std::min(1.f, t * 300.f);
                const float ph = kTwoPi * notes[n] * t;
                s[i] += (std::sin(ph) + 0.35f * std::sin(2.f * ph) + 0.15f * std::sin(3.f * ph)) * env * 0.12f;
            }
        }
    }
}

}  // namespace mud
