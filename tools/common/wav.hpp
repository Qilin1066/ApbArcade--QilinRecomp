#pragma once
// A 16-bit stereo PCM WAV file, written at the end of a run (--wav FILE).

#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <fstream>
#include <string>
#include <vector>

namespace qilin::tools {

// WAV rates are integers; chip clocks need not be. A rational source rate
// enables a windowed-sinc conversion instead of labelling fractional-rate
// samples with a slightly wrong timebase. Zero numerator means no conversion.
inline bool write_wav(const std::string& path, const std::vector<std::int16_t>& frames, std::uint32_t rate,
                      std::uint32_t source_numerator = 0, std::uint32_t source_denominator = 1) {
    if (!rate || !source_denominator || frames.size() % 2 != 0) return false;
    if (!source_numerator) { source_numerator = rate; source_denominator = 1; }
    const std::uint64_t target = static_cast<std::uint64_t>(rate) * source_denominator;
    const std::uint64_t input_count = frames.size() / 2;
    if (input_count > std::numeric_limits<std::uint64_t>::max() / target) return false;
    const std::uint64_t count = input_count * target / source_numerator;
    if (count > (std::numeric_limits<std::uint32_t>::max() - 36u) / 4u || rate > std::numeric_limits<std::uint32_t>::max() / 4u) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const auto data_bytes = static_cast<std::uint32_t>(count * 4);
    auto u32 = [&](std::uint32_t v) {
        const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
        out.write(b, 4);
    };
    auto u16 = [&](std::uint16_t v) {
        const char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
        out.write(b, 2);
    };
    out.write("RIFF", 4);
    u32(36 + data_bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);             // PCM
    u16(2);             // stereo
    u32(rate);
    u32(rate * 4);      // bytes a second
    u16(4);             // bytes a frame
    u16(16);            // bits a sample
    out.write("data", 4);
    u32(data_bytes);
    if (source_numerator == target) {
        for (const std::int16_t s : frames) u16(static_cast<std::uint16_t>(s));
    } else if (count) {
        constexpr int phases = 1024, radius = 16, taps = radius * 2;
        constexpr double pi = 3.14159265358979323846;
        const double cutoff = std::min(1.0, static_cast<double>(target) / source_numerator);
        std::vector<std::array<double, taps>> coefficients(phases);
        for (int phase = 0; phase < phases; ++phase) {
            double total = 0;
            for (int tap = 0; tap < taps; ++tap) {
                const double x = tap - radius + 1 - static_cast<double>(phase) / phases;
                const double sinc = std::abs(x) < 1e-12 ? cutoff : std::sin(pi * cutoff * x) / (pi * x);
                const double window = std::abs(x) >= radius ? 0 : 0.42 + 0.5 * std::cos(pi * x / radius) + 0.08 * std::cos(2 * pi * x / radius);
                total += coefficients[phase][tap] = sinc * window;
            }
            for (auto& coefficient : coefficients[phase]) coefficient /= total;
        }
        for (std::uint64_t sample = 0; sample < count; ++sample) {
            const auto position = sample * source_numerator;
            const auto index = static_cast<std::int64_t>(position / target);
            const auto phase = std::min(phases - 1, static_cast<int>(static_cast<double>(position % target) / static_cast<double>(target) * phases));
            double mixed[2]{};
            for (int tap = 0; tap < taps; ++tap) {
                const auto at = std::clamp<std::int64_t>(index + tap - radius + 1, 0, static_cast<std::int64_t>(input_count) - 1);
                for (int channel = 0; channel < 2; ++channel)
                    mixed[channel] += frames[static_cast<std::size_t>(at) * 2 + channel] * coefficients[phase][tap];
            }
            for (const double value : mixed)
                u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(std::lround(value), -32768L, 32767L))));
        }
    }
    return static_cast<bool>(out);
}

} // namespace qilin::tools
