// The lifter against MAME's T-11 core, instruction form by instruction form.
//
//   qilin_lifter_check <fixture-dir> [--trials N]
//
// Runs every function of the synthetic program qilin_lifter_fixture built
// (one per instruction form) from states made of edge values: 0, 1, 0x7F,
// 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0x8001, 0xFFFF and random words, in the
// registers, the flags and every word of RAM, with the trap vectors pointing
// at an RTI. Lifted code and MAME must agree on everything
// tools/difftest/compare.hpp compares. Needs no game files; runs in CI.

#include "compare.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

#include "t11_decoder.hpp"
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace qilin;
using namespace qilin::difftest;

constexpr std::array<std::uint16_t, 10> edges{0x0000, 0x0001, 0x007F, 0x0080, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0x8001, 0xFFFF};

State edge_state(std::mt19937& rng, std::span<const std::uint8_t> rom, const rt::Entry& entry) {
    State s;
    s.memory.rom = rom;
    const auto value = [&]() -> std::uint16_t {
        switch (rng() % 3) {
        case 0: return edges[rng() % edges.size()];
        case 1: return static_cast<std::uint16_t>(0x0200 + (rng() & 0x07ff));   // a RAM pointer, even or odd
        default: return static_cast<std::uint16_t>(rng());
        }
    };
    for (std::size_t a = 0; a < s.memory.ram.size(); a += 2) {
        const auto w = rng() % 2 ? edges[rng() % edges.size()] : static_cast<std::uint16_t>(rng());
        s.memory.ram[a] = static_cast<std::uint8_t>(w);
        s.memory.ram[a + 1] = static_cast<std::uint8_t>(w >> 8);
    }
    // Trap vectors: to the RTI stub at 0x8010, with a random new PSW (priority 7).
    for (const unsigned vector : {004u, 010u, 014u, 020u, 030u, 034u}) {
        s.memory.ram[vector] = 0x10;
        s.memory.ram[vector + 1] = 0x80;
        s.memory.ram[vector + 2] = static_cast<std::uint8_t>(0xe0 | (rng() & 0x0f));
        s.memory.ram[vector + 3] = 0;
    }
    for (int r = 0; r < 6; ++r) s.cpu.r[r] = value();
    s.cpu.r[6] = static_cast<std::uint16_t>(0x0c00 + (rng() & 0x01fe));
    s.cpu.r[6] = static_cast<std::uint16_t>(s.cpu.r[6] - 2);
    // Return addresses all round the stack pointer, so forms that move SP
    // (MOV (SP)+,x; @-(SP) ...) still return and are compared.
    for (int k = -8; k <= 8; ++k) {
        const auto a = static_cast<std::size_t>(s.cpu.r[6] + 2 * k);
        s.memory.ram[a] = sentinel & 0xff;
        s.memory.ram[a + 1] = sentinel >> 8;
    }
    s.cpu.r[7] = entry.pc;
    s.cpu.psw = static_cast<std::uint8_t>(0xe0 | (rng() & 0x0f));
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: qilin_lifter_check <fixture-dir> [--trials N]\n");
        return 2;
    }
    const int trials = argc >= 4 ? std::stoi(argv[3]) : 64;
    std::ifstream in(std::string(argv[1]) + "/program.bin", std::ios::binary);
    const std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (rom.size() != 0x90000) {
        std::fprintf(stderr, "qilin_lifter_check: can't read %s/program.bin\n", argv[1]);
        return 1;
    }
    long counts[6] = {};
    long functions = 0, exercised = 0, failed = 0;
    for (unsigned i = 0; i < rt::lifted_entry_count; ++i) {
        const auto& entry = rt::lifted_entries[i];
        if (entry.index != 0 || entry.pc < 0x8100) continue;   // the stubs aren't tests
        ++functions;
        std::mt19937 rng(entry.pc);
        bool matched = false, reported = false;
        for (int t = 0; t < trials; ++t) {
            const auto result = trial(edge_state(rng, rom, entry), entry);
            ++counts[static_cast<int>(result.outcome)];
            matched = matched || result.outcome == Outcome::match;
            if (result.outcome == Outcome::mismatch && !reported) {
                reported = true;
                ++failed;
                std::printf("MISMATCH at %04X trial %d:\n%s", entry.pc, t, result.detail.c_str());
            }
        }
        exercised += matched;
        if (!matched && std::getenv("QILIN_LIST_UNMATCHED")) {
            const auto w0 = static_cast<std::uint16_t>(rom[entry.pc] | rom[entry.pc + 1u] << 8);
            const auto w1 = static_cast<std::uint16_t>(rom[entry.pc + 2u] | rom[entry.pc + 3u] << 8);
            const auto w2 = static_cast<std::uint16_t>(rom[entry.pc + 4u] | rom[entry.pc + 5u] << 8);
            std::printf("never matched: %04X %s\n", entry.pc, t11::disassemble(t11::decode(w0, w1, w2), entry.pc).c_str());
        }
    }
    std::printf("lifter check: %ld instruction forms, %ld trials: %ld match, %ld mismatch (%ld set aside); "
                "%ld forms matched at least once.\n",
                functions, functions * trials, counts[0], counts[1], counts[2] + counts[3] + counts[4] + counts[5], exercised);
    return failed == 0 && exercised > functions * 9 / 10 ? 0 : 1;
}
