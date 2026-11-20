#pragma once
#include "system2_audio.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace qilin::sys2 {
// One game-thread producer records exact chip events. A worker owns the
// mixer and consumes bounded, ordered frame batches. No machine state is
// shared with it, and overload applies backpressure instead of dropping PCM.
class AudioWorker final : public Machine::SoundSink {
public:
    using Output = std::function<bool(std::span<const std::int16_t>)>;
    explicit AudioWorker(Output output);
    ~AudioWorker() override;
    bool submit(std::int64_t sound_time);
    bool drain();
    void finish();
    [[nodiscard]] std::string error() const;
    void pokey_write(int which, unsigned reg, std::uint8_t value, std::int64_t time) override;
    void ym_write(bool data, std::uint8_t value, std::int64_t time) override;
    void ym_reset(bool running, std::int64_t time) override;
    void mixer_write(std::uint8_t value, std::int64_t time) override;
    void speech_sample(std::int16_t value, std::int64_t main_cycle) override;
private:
    enum class Kind { pokey, ym, reset, mixer, speech };
    struct Event { Kind kind; int a, b; std::int64_t time; };
    struct Batch { std::vector<Event> events; std::int64_t until; };
    void record(Kind kind, int a, int b, std::int64_t time);
    void run();
    Audio audio_;
    Output output_;
    std::vector<Event> pending_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Batch> queue_;
    std::vector<std::vector<Event>> reusable_;
    std::atomic<bool> failed_{false};
    bool closing_ = false, busy_ = false;
    std::string error_;
    std::thread worker_;
};
} // namespace qilin::sys2
