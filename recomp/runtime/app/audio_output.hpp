#pragma once
#include <SDL3/SDL.h>
#include <cstdint>
#include <atomic>
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
    enum class State { unavailable, buffering, playing };
    [[nodiscard]] bool ok() const { return state_.load() != State::unavailable; }
    [[nodiscard]] State state() const { return state_.load(); }
    // May be called by the event thread; the worker performs the reopen.
    void request_reopen() { reopen_.store(true); }
    bool push(std::span<const std::int16_t> samples);
    bool pause();
private:
    void open();
    void unavailable();
    bool queue(std::span<const std::int16_t> samples);
    std::atomic<State> state_{State::unavailable};
    std::atomic<bool> reopen_{false};
    Uint64 retry_at_ = 0;
    SDL_AudioStream* stream_ = nullptr;
    bool started_ = false;
    float ratio_ = 1;
};
} // namespace qilin::app
