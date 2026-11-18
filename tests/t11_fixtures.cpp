#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {

void write_words(const std::filesystem::path& path, std::span<const std::uint16_t> words) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const auto word : words) {
        const std::array bytes{static_cast<std::uint8_t>(word), static_cast<std::uint8_t>(word >> 8)};
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write synthetic T-11 fixture");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: apb-t11-fixtures <output-directory>\n";
        return 2;
    }

    // Hand-authored DEC T-11 program. The three passes through the loop sum 3 + 2 + 1.
    constexpr std::uint16_t program[]{
        0012700, 3,         // MOV #3,R0
        0005001,            // CLR R1
        0060001,            // loop: ADD R0,R1
        0077002,            // SOB R0,loop
        0022701, 6,         // CMP #6,R1
        0001016,            // BNE fail
        0012702, 0xffff,    // MOV #-1,R2
        0062702, 1,         // ADD #1,R2
        0103011,            // BCC fail
        0001010,            // BNE fail
        0012703, 0x7fff,    // MOV #32767,R3
        0005203,            // INC R3
        0102004,            // BVC fail
        0100003,            // BPL fail
        0000401,            // BR pass
        0xffff,             // Unreachable data word
        0000000,            // pass: HALT
        0012704, 0xbad,     // fail: MOV #0xbad,R4
        0000000,            // HALT
    };

    constexpr std::uint16_t unsupported[]{0xffff};
    constexpr std::uint16_t overlap[]{0012700, 0, 0000776}; // BR into the immediate operand
    constexpr std::uint16_t out_of_range[]{0000776};       // BR to two bytes before the image

    try {
        const std::filesystem::path dir = argv[1];
        write_words(dir / "loop.bin", program);
        write_words(dir / "unsupported.bin", unsupported);
        write_words(dir / "overlap.bin", overlap);
        write_words(dir / "out-of-range.bin", out_of_range);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
