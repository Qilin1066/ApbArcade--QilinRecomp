#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

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

void write_pair(std::ofstream& map, const std::filesystem::path& dir, std::uint32_t offset,
                const std::string& name, std::span<const std::uint16_t> words) {
    const auto even_name = name + "-even.bin";
    const auto odd_name = name + "-odd.bin";
    std::ofstream even(dir / even_name, std::ios::binary | std::ios::trunc);
    std::ofstream odd(dir / odd_name, std::ios::binary | std::ios::trunc);
    for (const auto word : words) {
        even.put(static_cast<char>(word & 0xff));
        odd.put(static_cast<char>(word >> 8));
    }
    even.close();
    odd.close();
    if (!even || !odd) {
        throw std::runtime_error("cannot write synthetic System 2 byte lanes");
    }
    map << "0x" << std::hex << offset << " | " << even_name << " | " << odd_name << '\n';
}

void write_system2(const std::filesystem::path& dir) {
    constexpr std::uint16_t fixed_rom[]{
        0012737, 0x8123, 0x0200, // MOV #0x8123,@#0x0200
        0013737, 0x0200, 0x0202, // MOV @#0x0200,@#0x0202
        0013703, 0x0202,         // MOV @#0x0202,R3
        0010337, 0x0204,         // MOV R3,@#0x0204
        0062737, 1, 0x0204,      // ADD #1,@#0x0204
        0162737, 1, 0x0204,      // SUB #1,@#0x0204
        0105037, 0x0200,         // CLRB @#0x0200 (low byte)
        0105037, 0x0203,         // CLRB @#0x0203 (high byte)
        0005037, 0x0206,         // CLR @#0x0206
        0023737, 0xf000, 0x0204, // CMP @#0xf000,@#0x0204 (ROM word below RAM word)
        0000137, 0x4000,         // JMP @#0x4000
    };
    constexpr std::uint16_t bank0[]{
        0012700, 0x1111,         // MOV #0x1111,R0 at 0x4000
        0012737, 0x1c00, 0x147c, // Select bank 1 in window 0 through a mirror
    };
    constexpr std::uint16_t bank1[]{
        0012701, 0x2222,         // MOV #0x2222,R1 at the same PC in bank 1
        0000167, 8,              // JMP displacement(PC) to 0x4010
        0xffff,                  // Unreachable data
        0000167, 0xfff2,         // First instruction after switching at 0x4004, jump back to 0x4000
        0xffff,                  // Unreachable data
        0012737, 0x2c00, 0x147e, // 0x4010: select bank 2 in window 1
        0000137, 0x5ffc,         // JMP @#0x5ffc
    };
    // The last word of the MOV is fetched at 0x6000 under the old bank mapping.
    constexpr std::uint16_t bank1_tail[]{0012737, 0xbc00}; // Select bank 35 in window 1
    constexpr std::uint16_t bank2[]{0x147e};
    constexpr std::uint16_t bank35[]{
        0012702, 0x3333,         // 0x6002: MOV #0x3333,R2
        0005704,                 // TST R4
        0001411,                 // BEQ byte_clear
        0005304,                 // DEC R4
        0001403,                 // BEQ word_move
        0005037, 0x19fe,         // CLR @#0x19fe
        0000406,                 // BR done
        0012737, 0x8001, 0x1bfe, // word_move: MOV #0x8001,@#0x1bfe
        0000402,                 // BR done
        0105037, 0x1800,         // byte_clear: CLRB @#0x1800
        0,                       // done: HALT
    };
    constexpr std::uint16_t constant[]{0x8122};
    constexpr std::uint16_t unsupported_board[]{
        0010037, 0x1400,     // 0x8240: register value written to the bank selector
        0012737, 1, 0x1500,  // 0x8244: unsupported device write
        0022737, 0, 0x8000,  // 0x824a: Slapstic window is not a plain ROM data read
        0105037, 0x1801,     // 0x8250: high byte does not strobe the watchdog
        0022737, 0, 0x1800,  // 0x8254: explicit read needs the input port
    };

    std::ofstream map(dir / "system2.map", std::ios::trunc);
    map << "# Synthetic fixture\n";
    write_pair(map, dir, 0x08200, "fixed ROM", fixed_rom);
    write_pair(map, dir, 0x08240, "unsupported-board", unsupported_board);
    write_pair(map, dir, 0x0f000, "constant", constant);
    write_pair(map, dir, 0x10000, "bank0", bank0);
    write_pair(map, dir, 0x12000, "bank1", bank1);
    write_pair(map, dir, 0x13ffc, "bank1-tail", bank1_tail);
    write_pair(map, dir, 0x14000, "bank2", bank2);
    write_pair(map, dir, 0x56002, "bank35", bank35);
    map.close();
    if (!map) {
        throw std::runtime_error("cannot write synthetic System 2 ROM map");
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
        write_system2(dir);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
