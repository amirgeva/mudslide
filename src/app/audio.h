#pragma once

#include <SDL.h>

#include <vector>

namespace mud {

enum class Sfx { Fire, Boom, BigBoom, Crack, Split, Splat, Thud, Bore, Tick, Chime, Fanfare, Count };

// Synthesized sound effects mixed in the SDL audio callback. If no audio
// device is available the game simply runs silently.
class Audio {
public:
    ~Audio() { shutdown(); }

    bool init();
    void shutdown();
    void play(Sfx sfx, float volume = 1.f);
    void setMuted(bool muted) { muted_ = muted; }
    bool muted() const { return muted_; }

private:
    static void SDLCALL callback(void* user, Uint8* stream, int len);
    void synthesize();

    struct Voice {
        int sound;
        size_t pos;
        float volume;
    };

    SDL_AudioDeviceID device_ = 0;
    int rate_ = 44100;
    std::vector<float> sounds_[int(Sfx::Count)];
    std::vector<Voice> voices_;  // guarded by SDL_LockAudioDevice
    bool muted_ = false;
};

}  // namespace mud
