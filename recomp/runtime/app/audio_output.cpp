#include "audio_output.hpp"
#include "system2_audio.hpp"
#include <algorithm>

namespace qilin::app {
namespace {
constexpr int sample_rate = 55930;
constexpr float chip_ratio = static_cast<float>(sys2::Audio::sample_rate / sample_rate);
constexpr int frame_bytes = sample_rate * 4 / 60;
}
AudioOutput::AudioOutput() { open(); }
void AudioOutput::open() {
    if (stream_) SDL_DestroyAudioStream(stream_);
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, sample_rate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    started_ = false; ratio_ = chip_ratio;
    state_.store(stream_ ? State::buffering : State::unavailable);
    retry_at_ = SDL_GetTicks() + 1000;
}
void AudioOutput::unavailable() {
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr; started_ = false; ratio_ = chip_ratio;
    state_.store(State::unavailable);
    retry_at_ = SDL_GetTicks() + 1000;
}
AudioOutput::~AudioOutput() { if (stream_) SDL_DestroyAudioStream(stream_); }

bool AudioOutput::push(std::span<const std::int16_t> samples) {
    if (reopen_.exchange(false) || (!stream_ && SDL_GetTicks() >= retry_at_)) open();
    if (stream_ && !queue(samples)) unavailable();
    // Keep advancing the chips during an outage, so restored playback is
    // current gameplay rather than seconds of stale buffered sound.
    return true;
}
bool AudioOutput::queue(std::span<const std::int16_t> samples) {
    if (!stream_) return false;
    const int queued = SDL_GetAudioStreamQueued(stream_);
    if (queued < 0) return false;
    if (started_ && queued == 0) {
        // Refill after a stall rather than repeatedly starving on single frames.
        if (!SDL_PauseAudioStreamDevice(stream_)) return false;
        started_ = false;
        state_.store(State::buffering);
    }
    // A disconnected/stalled device must not accumulate unlimited memory.
    // Report failure to the player instead of silently dropping game audio.
    if (queued > sample_rate * 4 * 2) { SDL_SetError("Audio device stopped consuming samples"); return false; }
    if (!SDL_PutAudioStreamData(stream_, samples.data(), static_cast<int>(samples.size_bytes()))) return false;
    if (!started_) {
        ratio_ = chip_ratio;
        if (!SDL_SetAudioStreamFrequencyRatio(stream_, ratio_)) return false;
        if (queued + static_cast<int>(samples.size_bytes()) >= 2 * frame_bytes) {
            if (!SDL_ResumeAudioStreamDevice(stream_)) return false;
            started_ = true;
            state_.store(State::playing);
        }
        return true;
    }
    // Correct device/display clock drift smoothly instead of discarding
    // complete frames of PCM. The exact chip mix remains unchanged.
    const float target = chip_ratio * std::clamp(1.0f + 0.003f * (static_cast<float>(queued) / frame_bytes - 2.0f), 0.98f, 1.02f);
    ratio_ += (target - ratio_) * 0.02f;
    return SDL_SetAudioStreamFrequencyRatio(stream_, ratio_);
}
bool AudioOutput::pause() {
    if (!stream_) return true;
    started_ = false; ratio_ = chip_ratio;
    if (!SDL_PauseAudioStreamDevice(stream_) || !SDL_ClearAudioStream(stream_) || !SDL_SetAudioStreamFrequencyRatio(stream_, ratio_)) {
        unavailable(); return false;
    }
    state_.store(State::buffering);
    return true;
}
} // namespace qilin::app
