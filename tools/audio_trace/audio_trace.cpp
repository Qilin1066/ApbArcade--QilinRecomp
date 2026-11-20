// Re-render an original MAME/native bus trace through the native sound chips.
// CPU scheduling and controller inputs are absent from this comparison.
#include "system2_audio.hpp"
#include "wav.hpp"
#include <charconv>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {
template<class T> bool number(const std::string& text, T& out, int base = 10) {
    if (text.empty()) return false;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out, base);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}
}
int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: qilin_audio_trace TRACE OUTPUT.wav STEREO_FRAMES\n"); return 2;
    }
    std::int64_t samples = 0;
    if (!number(std::string(argv[3]), samples) || samples <= 0 || samples > (14318181LL * 3600 + 255) / 256) {
        std::fprintf(stderr, "invalid frame count (maximum one hour)\n"); return 2;
    }
    std::ifstream trace(argv[1]);
    if (!trace) { std::fprintf(stderr, "cannot open trace\n"); return 1; }
    using namespace qilin;
    const std::vector<std::uint8_t> main_rom(0x90000), sound_rom(0x10000);
    sys2::Machine machine(main_rom, sound_rom);
    sys2::Audio audio;
    machine.sound_sink = &audio;
    const auto until = samples * sys2::Audio::cycles_per_sample;
    std::int64_t previous = 0;
    std::size_t line_number = 0, writes = 0, all_writes = 0;
    bool ended = false;
    for (std::string line; std::getline(trace, line);) {
        ++line_number;
        if (line.starts_with("# end ")) {
            std::int64_t end = 0;
            if (ended || !number(line.substr(6), end) || end < previous || end < sys2::main_cycles_at(until)) {
                std::fprintf(stderr, "missing coverage in end marker\n"); return 1;
            }
            ended = true;
            continue;
        }
        if (ended) { std::fprintf(stderr, "data after end marker\n"); return 1; }
        std::istringstream fields(line);
        std::string kind, address_text, value_text, time_text, extra;
        unsigned address = 0, value = 0;
        std::int64_t time = 0;
        if (!(fields >> kind >> address_text >> value_text >> time_text) || fields >> extra ||
            (kind != "r" && kind != "w") || !number(address_text, address, 16) || address > 0xffff ||
            !number(value_text, value, 16) || value > 255 || !number(time_text, time) ||
            time < previous || time > 3601LL * 10000000 ||
            (address & 0x1fff) < 0x1800 || address >= 0x4000) {
            std::fprintf(stderr, "invalid or unordered trace at line %zu\n", line_number); return 1;
        }
        previous = time;
        machine.sound_time = sys2::sound_cycles_at(time);
        if (kind == "w") ++all_writes;
        if (machine.sound_time > until) continue;
        machine.main_time = time;
        // Speech is a board device; advance it before chip writes can mix.
        machine.advance_speech(machine.sound_time);
        if (kind == "w") {
            machine.sound_write(static_cast<std::uint16_t>(address), static_cast<std::uint8_t>(value));
            ++writes;
        }
    }
    if (!trace.eof() && trace.fail()) { std::fprintf(stderr, "trace read failed\n"); return 1; }
    if (!ended || all_writes == 0) {
        std::fprintf(stderr, "empty or truncated trace\n"); return 1;
    }
    machine.advance_speech(until);
    audio.advance(until);
    if (audio.frames().size() != static_cast<std::size_t>(samples) * 2 || !tools::write_wav(argv[2], audio.frames(), 55930, 14318181, 256)) {
        std::fprintf(stderr, "could not write complete WAV\n"); return 1;
    }
    std::printf("Replayed %zu writes into %lld stereo frames\n", writes, static_cast<long long>(samples));
}
