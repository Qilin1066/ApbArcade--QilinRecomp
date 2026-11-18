// Differential test: every lifted 6502 function of the sound program against
// MAME's 6502 core.
//
//   qilin_sound_difftest <rom-dir> [--trials N] [--seed S] [--only XXXX]
//
// Each trial builds one random state (A, X, Y, the flags, decimal mode
// included, the stack pointer, 16 KiB of RAM and device space), calls the
// function as JSR would (or enters an interrupt handler as an interrupt
// would), and runs it twice: in MAME's core (third_party/mame-m6502) until it
// returns, and as lifted C++ in time slices of random length, so lifted code
// also yields and resumes mid-function. The two must agree on every register,
// the cycles used, and the whole bus trace in order, whole call trees
// included. A trial is set aside, not failed, when the original doesn't
// return within the step limit (a wait on a device the test doesn't model)
// or reaches code nothing was lifted for.
//
// Needs the ROMs and a build with -DQILIN_SOUND_GEN_DIR (the output of
// qilin lift --cpu sound).

#include "compare6502.hpp"

#include "romset.hpp"
#include "sound_lifted.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <optional>
#include <print>
#include <random>
#include <string_view>

namespace {

using namespace qilin;
using namespace qilin::difftest6502;

State random_state(std::mt19937& rng, std::span<const std::uint8_t> rom, std::uint16_t entry, bool interrupt) {
    State s;
    for (std::size_t a = 0; a < 0x4000; ++a) s.memory.bytes[a] = static_cast<std::uint8_t>(rng());
    std::copy(rom.begin() + 0x4000, rom.end(), s.memory.bytes.begin() + 0x4000);
    const auto value = [&]() -> std::uint8_t {
        switch (rng() % 4) {
        case 0: return static_cast<std::uint8_t>(rng() % 16);     // a small count or index
        case 1: return static_cast<std::uint8_t>(0x80 | rng());   // negative
        default: return static_cast<std::uint8_t>(rng());
        }
    };
    s.cpu.a = value();
    s.cpu.x = value();
    s.cpu.y = value();
    // Decimal mode one trial in four: the program clears it, but must survive either.
    s.cpu.p = static_cast<std::uint8_t>((rng() & 0xc7) | 0x30 | (rng() % 4 == 0 ? rt6502::F_D : 0));
    s.cpu.sp = static_cast<std::uint16_t>(0x100 | (0x80 + rng() % 0x80));
    push_return(s, interrupt);
    s.cpu.pc = entry;
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::print(stderr, "usage: qilin_sound_difftest <rom-dir> [--trials N] [--seed S] [--only XXXX]\n");
        return 2;
    }
    int trials = 64;
    unsigned seed = 1;
    std::optional<std::uint16_t> only;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const std::string_view value = argv[i + 1];
        unsigned number = 0;
        std::from_chars(value.data(), value.data() + value.size(), number, flag == "--only" ? 16 : 10);
        if (flag == "--trials") trials = static_cast<int>(number);
        else if (flag == "--seed") seed = number;
        else if (flag == "--only") only = static_cast<std::uint16_t>(number);
    }
    const auto set = rom::RomSet::load(argv[1]);
    if (!set) {
        std::print(stderr, "qilin_sound_difftest: {}\n", set.error());
        return 1;
    }
    const auto rom = set->region(rom::Region::sound_cpu);
    const auto vector = [&](unsigned at) { return static_cast<std::uint16_t>(rom[at] | (rom[at + 1] << 8)); };
    const std::uint16_t nmi = vector(0xfffa), irq = vector(0xfffe);

    std::map<Outcome, long> totals;
    long functions = 0, exercised = 0, failed = 0, yields = 0;
    for (unsigned i = 0; i < rt6502::lifted_entry_count; ++i) {
        const auto& entry = rt6502::lifted_entries[i];
        if (entry.index != 0 || (only && entry.pc != *only)) continue;
        ++functions;
        std::mt19937 rng(seed * 7919u + entry.pc * 31u);
        bool any_match = false, reported = false;
        for (int t = 0; t < trials; ++t) {
            const auto start = random_state(rng, rom, entry.pc, entry.pc == nmi || entry.pc == irq);
            const auto slice = static_cast<std::int32_t>(1 + rng() % 400);
            const auto result = trial(start, entry.fn, slice);
            ++totals[result.outcome];
            yields += result.yields;
            any_match = any_match || result.outcome == Outcome::match;
            if (result.outcome == Outcome::mismatch && !reported) {
                reported = true;
                ++failed;
                std::print("MISMATCH snd_{:04X} trial {}:\n{}", entry.pc, t, result.detail);
            }
        }
        exercised += any_match;
    }
    std::print("{} functions, {} trials: {} match, {} mismatch; set aside: {} didn't return in {} steps, {} reached "
               "unlifted code, {} ran into RAM or data. The lifted code yielded and resumed {} times.\n",
               functions, functions * trials, totals[Outcome::match], totals[Outcome::mismatch], totals[Outcome::skip_runaway],
               step_limit, totals[Outcome::skip_unlifted], totals[Outcome::skip_left], yields);
    std::print("{} functions completed at least one matching trial; {} had a mismatch.\n", exercised, failed);
    return failed == 0 ? 0 : 1;
}
