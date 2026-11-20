#include "audio_worker.hpp"
#include <exception>
#include <stdexcept>

namespace qilin::sys2 {
AudioWorker::AudioWorker(Output output) : output_(std::move(output)) {
    pending_.reserve(512);
    worker_ = std::thread([this] { run(); });
}
AudioWorker::~AudioWorker() { finish(); }
void AudioWorker::record(Kind kind, int a, int b, std::int64_t time) {
    if (!failed_.load()) pending_.push_back({kind, a, b, time});
}
void AudioWorker::pokey_write(int which, unsigned reg, std::uint8_t value, std::int64_t time) {
    record(Kind::pokey, which * 16 + static_cast<int>(reg), value, time);
}
void AudioWorker::ym_write(bool data, std::uint8_t value, std::int64_t time) { record(Kind::ym, data ? 1 : 0, value, time); }
void AudioWorker::ym_reset(bool running, std::int64_t time) { record(Kind::reset, running ? 1 : 0, 0, time); }
void AudioWorker::mixer_write(std::uint8_t value, std::int64_t time) { record(Kind::mixer, value, 0, time); }
void AudioWorker::speech_sample(std::int16_t value, std::int64_t time) { record(Kind::speech, value, 0, time); }

bool AudioWorker::submit(std::int64_t sound_time) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return failed_.load() || closing_ || queue_.size() < 4; });
    if (failed_.load() || closing_) { pending_.clear(); return false; }
    queue_.push_back({{}, sound_time});
    queue_.back().events.swap(pending_);
    if (!reusable_.empty()) { pending_.swap(reusable_.back()); reusable_.pop_back(); }
    else pending_.reserve(512);
    changed_.notify_all();
    return true;
}
bool AudioWorker::drain() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return failed_.load() || (queue_.empty() && !busy_); });
    return !failed_.load();
}
void AudioWorker::finish() {
    { std::lock_guard lock(mutex_); closing_ = true; }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}
std::string AudioWorker::error() const { std::lock_guard lock(mutex_); return error_; }

void AudioWorker::run() {
    try {
        for (;;) {
            Batch batch;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return closing_ || !queue_.empty(); });
                if (queue_.empty()) return;
                batch = std::move(queue_.front()); queue_.pop_front(); busy_ = true;
                changed_.notify_all();
            }
            for (const auto& e : batch.events) {
                switch (e.kind) {
                case Kind::pokey: audio_.pokey_write(e.a / 16, static_cast<unsigned>(e.a % 16), static_cast<std::uint8_t>(e.b), e.time); break;
                case Kind::ym: audio_.ym_write(e.a != 0, static_cast<std::uint8_t>(e.b), e.time); break;
                case Kind::reset: audio_.ym_reset(e.a != 0, e.time); break;
                case Kind::mixer: audio_.mixer_write(static_cast<std::uint8_t>(e.a), e.time); break;
                case Kind::speech: audio_.speech_sample(static_cast<std::int16_t>(e.a), e.time); break;
                }
            }
            audio_.advance(batch.until);
            if (!output_(audio_.frames())) throw std::runtime_error("Audio output rejected samples");
            audio_.frames().clear(); batch.events.clear();
            {
                std::lock_guard lock(mutex_);
                reusable_.push_back(std::move(batch.events)); busy_ = false;
            }
            changed_.notify_all();
        }
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_); error_ = e.what(); failed_.store(true); queue_.clear(); busy_ = false;
    } catch (...) {
        std::lock_guard lock(mutex_); error_ = "Unknown mixer failure"; failed_.store(true); queue_.clear(); busy_ = false;
    }
    changed_.notify_all();
}
} // namespace qilin::sys2
