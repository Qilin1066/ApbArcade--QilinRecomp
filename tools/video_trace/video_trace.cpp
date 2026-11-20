// Render original MAME's captured per-band video state; no CPU is run.
#include "system2_video.hpp"
#include "gfx.hpp"
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
std::uint32_t read(std::istream& input, int bytes) {
    std::uint32_t result = 0;
    for (int i = 0; i < bytes; ++i) {
        const int ch = input.get();
        if (ch == EOF) throw std::runtime_error("Truncated video capture");
        result |= static_cast<std::uint32_t>(ch) << (i * 8);
    }
    return result;
}
struct Work {
    std::unique_ptr<qilin::sys2::Frame> image = std::make_unique<qilin::sys2::Frame>();
    unsigned next_line = 0;
    bool finished = false;
};
}
int main(int argc, char** argv) {
    using namespace qilin;
    if (argc != 4) { std::fprintf(stderr, "usage: qilin_video_trace ROM_DIR CAPTURE OUTPUT_PREFIX\n"); return 2; }
    try {
        std::ifstream capture(argv[2], std::ios::binary);
        std::string magic(8, '\0');
        if (!capture.read(magic.data(), 8) || magic != std::string("APBVID1\0", 8)) throw std::runtime_error("Invalid video capture header");
        const auto set = rom::RomSet::load(argv[1]);
        if (!set) throw std::runtime_error(set.error());
        const sys2::Graphics graphics(*set);
        std::map<std::uint32_t, Work> work;
        std::uint32_t bands = 0, pictures = 0;
        for (;;) {
            const int kind = capture.get();
            if (kind == 'E') {
                const auto expected_bands = read(capture, 4), expected_pictures = read(capture, 4);
                if (expected_bands != bands || expected_pictures != pictures || !pictures || capture.peek() != EOF ||
                    work.size() != pictures) throw std::runtime_error("Incomplete video capture");
                break;
            }
            if (kind != 'B' && kind != 'P') throw std::runtime_error("Invalid or missing video record");
            const auto frame = read(capture, 4);
            if (frame > 1000000 || (!work.contains(frame) && work.size() >= 1000)) throw std::runtime_error("Invalid frame count");
            auto& current = work[frame];
            if (current.finished) throw std::runtime_error("Data after completed frame");
            if (kind == 'B') {
                const auto first = read(capture, 2), last = read(capture, 2);
                const auto xscroll = read(capture, 2), yscroll = read(capture, 2);
                const auto sy = static_cast<std::int32_t>(read(capture, 4));
                if (first != current.next_line || last < first || last >= 384 || sy < -511 || sy > 1023)
                    throw std::runtime_error("Missing, overlapping or invalid scanline band");
                sys2::VideoPages pages;
                for (auto& page : pages) for (auto& word : page) word = static_cast<std::uint16_t>(read(capture, 2));
                sys2::render(sys2::RenderView{pages, static_cast<int>(xscroll >> 6), sy, {xscroll & 15, yscroll & 15}},
                             graphics, *current.image, static_cast<int>(first), static_cast<int>(last));
                current.next_line = last + 1;
                ++bands;
            } else {
                if (current.next_line != 384) throw std::runtime_error("Frame has missing scanlines");
                sys2::PaletteRam palette;
                for (unsigned i = 0; i < 256; ++i) palette.write_word(static_cast<std::uint16_t>(0x1000 + i * 2), static_cast<std::uint16_t>(read(capture, 2)));
                const gfx::Image image{384, 512, sys2::to_rgba(*current.image, palette)};
                if (!gfx::write_png(std::string(argv[3]) + "-" + std::to_string(frame) + ".png", image)) throw std::runtime_error("Cannot write picture");
                current.finished = true;
                current.image.reset();
                ++pictures;
            }
        }
        std::printf("Replayed %u bands into %u pictures\n", bands, pictures);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "qilin_video_trace: %s\n", error.what()); return 1;
    }
}
