#include "profile.hpp"
#include "persistence.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace qilin::profile {
void Profiler::add(Stage stage, Clock::duration duration) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    if (ns < 0) return;
    std::lock_guard lock(mutex_);
    auto& stat = stats_[static_cast<std::size_t>(stage)];
    ++stat.count; stat.total += ns; stat.maximum = std::max<std::int64_t>(stat.maximum, ns);
    constexpr std::size_t capacity = 4096;
    if (stat.samples.size() < capacity) stat.samples.push_back(ns);
    else {
        // Bounded deterministic reservoir; never consumes the game's RNG.
        auto hash = stat.count * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 29;
        const auto index = hash % stat.count;
        if (index < capacity) stat.samples[static_cast<std::size_t>(index)] = ns;
    }
}
void Profiler::begin_frame(Clock::time_point now) {
    if (began_ != Clock::time_point{}) add(Stage::interval, now - began_);
    if (returned_ != Clock::time_point{}) add(Stage::simulation, now - returned_);
    began_ = now;
}
bool Profiler::save(const std::string& path, const std::string& context, std::string* error) const {
    std::lock_guard lock(mutex_);
    constexpr std::array<const char*, static_cast<std::size_t>(Stage::count)> names{
        "simulation_including_video", "video_cpu", "audio_worker", "audio_submit_and_wait", "controller_poll", "input_record_io", "color_conversion",
        "upload", "menu_ui", "present_and_wait", "pacing_sleep", "frame_interval"};
    std::ostringstream out;
    out << "APB performance report\nContext: " << context
        << "\nAll times in milliseconds. Mean/max cover all samples; median/p95 use a bounded reservoir (up to 4096)."
        << "\nSimulation includes video; do not sum nested stages. Present/pacing can include intentional waiting.\n"
        << "stage\tcount\ttotal_ms\tmean_ms\tmedian_ms\tp95_ms\tmax_ms\tsamples\n" << std::fixed << std::setprecision(4);
    for (std::size_t i = 0; i < stats_.size(); ++i) {
        const auto& stat = stats_[i];
        if (!stat.count) continue;
        auto sample = stat.samples;
        std::sort(sample.begin(), sample.end());
        out << names[i] << '\t' << stat.count << '\t' << static_cast<double>(stat.total / 1e6L) << '\t' << static_cast<double>(stat.total / stat.count / 1e6L)
            << '\t' << sample[(sample.size() - 1) / 2] / 1e6 << '\t' << sample[(sample.size() * 95 + 99) / 100 - 1] / 1e6
            << '\t' << stat.maximum / 1e6 << '\t' << sample.size() << '\n';
    }
    return persist::replace(path, out.str(), error);
}
} // namespace qilin::profile
