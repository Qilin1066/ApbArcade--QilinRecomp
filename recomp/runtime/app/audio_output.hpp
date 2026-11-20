#pragma once
#include <SDL3/SDL.h>
#include <cstdint>
#include <span>

namespace qilin::app {
// Owned by the audio worker during play. Call pause only after draining
// the worker, and destroy only after joining it.
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;
    [[nodiscard]] bool ok() const { return stream_ != nullptr; }
    bool push(std::span<const std::int16_t> samples);
    bool pause();
private:
    SDL_AudioStream* stream_ = nullptr;
    bool started_ = false;
    float ratio_ = 1;
};
} // namespace qilin::app
