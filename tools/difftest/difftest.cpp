// Differential test: every lifted T-11 function against MAME's T-11 core.
//
//   qilin_difftest <rom-dir> [--trials N] [--seed S] [--only XXXX]
//
// Each trial builds one random state (registers, flags, 16 KiB of RAM, the
// banks), calls the function as JSR PC would, and runs it twice: in MAME's
// core (third_party/mame-t11) until it returns, and as lifted C++. The two
// must agree on every register, the PSW, the cycles used, and the whole bus
// trace in order (fetches, reads and writes with their addresses and values),
// whole call trees included. A trial is set aside, not failed, when the
// original doesn't return within the step limit, waits for an interrupt, or
// reaches code nothing was lifted for.
//
// Needs the ROMs and a build with -DQILIN_GEN_DIR (the output of qilin lift).

#include "compare.hpp"

#include "romset.hpp"

#include <charconv>
#include <cstdio>
#include <exception>
#include <format>
#include <map>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>

namespace {

using namespace qilin;
using namespace qilin::difftest;

State random_state(std::mt19937& rng, std::span<const std::uint8_t> rom, const rt::Entry& entry,
                   const std::vector<std::uint8_t>& code_banks) {
    State s;
    s.memory.rom = rom;
    for (auto& byte : s.memory.ram) byte = static_cast<std::uint8_t>(rng());
    // Window 0 holds the function's bank, or a code bank; window 1 anything.
    s.memory.bank[0] = entry.pc < 0x8000 ? entry.bank : code_banks[rng() % code_banks.size()];
    s.memory.bank[1] = static_cast<std::uint8_t>(rng() % 64);
    const auto value = [&]() -> std::uint16_t {
        switch (rng() % 4) {
        case 0: return static_cast<std::uint16_t>(rng() & 0x0ffe);           // a RAM pointer
        case 1: return static_cast<std::uint16_t>(rng() % 16);               // a small count
        case 2: return static_cast<std::uint16_t>((rng() & 0x0ffe) | 1);     // odd
        default: return static_cast<std::uint16_t>(rng());
        }
    };
    for (int r = 0; r < 6; ++r) s.cpu.r[r] = value();
    s.cpu.r[6] = static_cast<std::uint16_t>(0x0c00 + (rng() & 0x01fe));      // stack in RAM
    // Called as JSR PC: the return address is on top of the stack.
    s.cpu.r[6] = static_cast<std::uint16_t>(s.cpu.r[6] - 2);
    s.memory.ram[s.cpu.r[6]] = sentinel & 0xff;
    s.memory.ram[s.cpu.r[6] + 1u] = sentinel >> 8;
    s.cpu.r[7] = entry.pc;
    s.cpu.psw = static_cast<std::uint8_t>(0xe0 | (rng() & 0x0f));            // priority 7, random flags
    return s;
}

std::string entry_name(const rt::Entry& e) {
    return e.pc >= 0x8000 ? std::format("{:04X}", e.pc) : std::format("{:04X}/b{}", e.pc, e.bank);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::print(stderr, "usage: qilin_difftest <rom-dir> [--trials N] [--seed S] [--only XXXX]\n");
        return 2;
    }
    int trials = 8;
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
        std::print(stderr, "qilin_difftest: {}\n", set.error());
        return 1;
    }
    const auto rom = set->region(rom::Region::main_cpu);
    std::vector<std::uint8_t> code_banks;
    for (unsigned i = 0; i < rt::lifted_entry_count; ++i) {
        const auto& e = rt::lifted_entries[i];
        if (e.pc < 0x8000 && std::ranges::find(code_banks, e.bank) == code_banks.end()) code_banks.push_back(e.bank);
    }

    std::map<Outcome, long> totals;
    long functions = 0, exercised = 0, failed = 0;
    for (unsigned i = 0; i < rt::lifted_entry_count; ++i) {
        const auto& entry = rt::lifted_entries[i];
        if (entry.index != 0 || (only && entry.pc != *only)) continue;
        ++functions;
        std::mt19937 rng(seed * 7919u + entry.pc * 31u + entry.bank);
        bool any_match = false, reported = false;
        for (int t = 0; t < trials; ++t) {
            const auto start = random_state(rng, rom, entry, code_banks);
            const auto result = trial(start, entry);
            ++totals[result.outcome];
            any_match = any_match || result.outcome == Outcome::match;
            if (result.outcome == Outcome::mismatch && !reported) {
                reported = true;
                ++failed;
                std::print("MISMATCH fn {} trial {}:\n{}", entry_name(entry), t, result.detail);
            }
        }
        exercised += any_match;
    }
    std::print("{} functions, {} trials: {} match, {} mismatch; set aside: {} didn't return in {} steps, {} waited for an "
               "interrupt, {} reached unlifted code, {} jumped into RAM.\n",
               functions, functions * trials, totals[Outcome::match], totals[Outcome::mismatch], totals[Outcome::skip_runaway],
               step_limit, totals[Outcome::skip_wait], totals[Outcome::skip_unlifted], totals[Outcome::skip_left]);
    std::print("{} functions completed at least one matching trial; {} had a mismatch.\n", exercised, failed);
    return failed == 0 ? 0 : 1;
}
