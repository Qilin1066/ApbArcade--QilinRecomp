#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace qilin::profile {
enum class Stage { simulation, video, audio_worker, audio_submit, input, recording, color, upload, ui, present, pacing, interval, count };
class Profiler {
public:
    using Clock = std::chrono::steady_clock;
    void add(Stage stage, Clock::duration duration);
    void begin_frame(Clock::time_point now);
    void end_frame(Clock::time_point now) { returned_ = now; }
    void reset_interval() { began_ = {}; returned_ = {}; }
    bool save(const std::string& path, const std::string& context, std::string* error = nullptr) const;
private:
    struct Stats {
        std::uint64_t count = 0;
        long double total = 0;
        std::int64_t maximum = 0;
        std::vector<std::int64_t> samples;
    };
    mutable std::mutex mutex_;
    std::array<Stats, static_cast<std::size_t>(Stage::count)> stats_;
    Clock::time_point began_{}, returned_{}; // main-thread frame boundaries only
};
class Scope {
public:
    Scope(Profiler* profiler, Stage stage) : profiler_(profiler), stage_(stage) { if (profiler_) began_ = Profiler::Clock::now(); }
    ~Scope() { if (profiler_) profiler_->add(stage_, Profiler::Clock::now() - began_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Profiler* profiler_;
    Stage stage_;
    Profiler::Clock::time_point began_{};
};
class FrameMark {
public:
    explicit FrameMark(Profiler* profiler) : profiler_(profiler) { if (profiler_) profiler_->begin_frame(Profiler::Clock::now()); }
    ~FrameMark() { if (profiler_) profiler_->end_frame(Profiler::Clock::now()); }
    FrameMark(const FrameMark&) = delete;
    FrameMark& operator=(const FrameMark&) = delete;
private:
    Profiler* profiler_;
};
} // namespace qilin::profile
