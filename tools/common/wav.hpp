#pragma once
// A 16-bit stereo PCM WAV file, written at the end of a run (--wav FILE).

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace qilin::tools {

inline bool write_wav(const std::string& path, const std::vector<std::int16_t>& frames, std::uint32_t rate) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const auto data_bytes = static_cast<std::uint32_t>(frames.size() * 2);
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
    for (const std::int16_t s : frames) u16(static_cast<std::uint16_t>(s));
    return static_cast<bool>(out);
}

} // namespace qilin::tools
