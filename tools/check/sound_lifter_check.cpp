// The 6502 lifter against MAME's 6502 core, instruction form by instruction form.
//
//   qilin_sound_lifter_check <fixture-dir> [--trials N]
//
// Runs every function of the synthetic program qilin_sound_lifter_fixture
// built (one per instruction form) from states made of edge values (0, 1,
// 0x7F, 0x80, 0xFE, 0xFF and random bytes) in A, X, Y and RAM, every flag
// combination, decimal mode included, and zero-page pointers into RAM and
// ROM, in time slices of random length so the lifted code also yields and
// resumes. Lifted code and MAME must agree on everything
// tools/difftest6502/compare6502.hpp compares. Needs no game files; runs in CI.

#include "compare6502.hpp"
#include "sound_lifted.hpp"

#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace qilin;
using namespace qilin::difftest6502;

constexpr std::array<std::uint8_t, 6> edges{0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};

State edge_state(std::mt19937& rng, const std::vector<std::uint8_t>& program, std::uint16_t entry, bool interrupt) {
    State s;
    std::copy(program.begin() + 0x4000, program.end(), s.memory.bytes.begin() + 0x4000);
    const auto value = [&]() -> std::uint8_t {
        return rng() % 2 ? edges[rng() % edges.size()] : static_cast<std::uint8_t>(rng());
    };
    for (std::size_t a = 0; a < 0x4000; ++a) s.memory.bytes[a] = value();
    // The forms' zero-page pointers ($20-$21 + X, $30-$31): into RAM or the ROM data area.
    for (unsigned zp = 0x20; zp < 0x40; zp += 2) {
        const bool rom = rng() % 3 == 0;
        s.memory.bytes[zp] = static_cast<std::uint8_t>(rng() % 2 ? 0xF0 + rng() % 16 : rng());
        s.memory.bytes[zp + 1] = static_cast<std::uint8_t>(rom ? 0x70 : 0x02 + rng() % 0x3c);
    }
    s.memory.bytes[0x40] = static_cast<std::uint8_t>(rng() % 6);   // the RTS table's index: 4 and 5 are out of range
    s.cpu.a = value();
    s.cpu.x = value();
    s.cpu.y = value();
    s.cpu.p = static_cast<std::uint8_t>((rng() & 0xcf) | 0x30);
    s.cpu.sp = static_cast<std::uint16_t>(0x100 | (0x80 + rng() % 0x80));
    push_return(s, interrupt);
    s.cpu.pc = entry;
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: qilin_sound_lifter_check <fixture-dir> [--trials N]\n");
        return 2;
    }
    int trials = 96;
    if (argc >= 4 && std::string_view(argv[2]) == "--trials") {
        const std::string_view value = argv[3];
        std::from_chars(value.data(), value.data() + value.size(), trials);
    }
    std::ifstream file(std::string(argv[1]) + "/program.bin", std::ios::binary);
    const std::vector<std::uint8_t> program{std::istreambuf_iterator<char>(file), {}};
    if (program.size() != 0x10000) {
        std::fprintf(stderr, "qilin_sound_lifter_check: no program.bin in %s\n", argv[1]);
        return 1;
    }
    const auto vector = [&](unsigned at) { return static_cast<std::uint16_t>(program[at] | (program[at + 1] << 8)); };
    const std::uint16_t nmi = vector(0xfffa), irq = vector(0xfffe);

    long matched = 0, mismatched = 0, set_aside = 0, yields = 0, forms = 0, exercised = 0;
    std::vector<std::uint16_t> unmatched;
    for (unsigned i = 0; i < rt6502::lifted_entry_count; ++i) {
        const auto& entry = rt6502::lifted_entries[i];
        if (entry.index != 0) continue;
        ++forms;
        std::mt19937 rng(entry.pc * 2654435761u);
        bool any = false, reported = false;
        for (int t = 0; t < trials; ++t) {
            const auto start = edge_state(rng, program, entry.pc, entry.pc == nmi || entry.pc == irq);
            const auto result = trial(start, entry.fn, static_cast<std::int32_t>(1 + rng() % 24));
            yields += result.yields;
            switch (result.outcome) {
            case Outcome::match: ++matched; any = true; break;
            case Outcome::mismatch:
                ++mismatched;
                if (!reported) {
                    reported = true;
                    std::printf("MISMATCH %04X  %s, trial %d:\n%s", entry.pc,
                                m6502::disassemble(m6502::decode(program[entry.pc], program[entry.pc + 1u], program[entry.pc + 2u]),
                                                   entry.pc).c_str(),
                                t, result.detail.c_str());
                }
                break;
            default: ++set_aside; break;
            }
        }
        exercised += any;
        if (!any) unmatched.push_back(entry.pc);
    }
    std::printf("qilin_sound_lifter_check: %ld of %ld forms completed; %ld trials: %ld match, %ld mismatch, %ld set aside; "
                "%ld yields and resumes\n",
                exercised, forms, matched + mismatched + set_aside, matched, mismatched, set_aside, yields);
    for (const auto pc : unmatched) {
        std::printf("  never completed: %04X  %s\n", pc,
                    m6502::disassemble(m6502::decode(program[pc], program[pc + 1u], program[pc + 2u]), pc).c_str());
    }
    return mismatched == 0 && exercised * 10 > forms * 9 ? 0 : 1;
}
