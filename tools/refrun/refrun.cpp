// Reference run: the game on MAME's own CPU cores (third_party/mame-t11 and
// third_party/mame-m6502) and this project's System 2 board model
// (recomp/runtime/system2_machine.*), with every executed address recorded.
//
//   qilin_refrun <rom-dir> [--frames N] [--quantum CYCLES] [--coin FRAME]... [--press MASK:FRAME]...
//                [--pedal VALUE] [--steer VALUE] [--seeds FILE] [--seeds-out FILE]
//
// --coin closes the left coin switch for 3 frames; --press pulls the IN0
// bits in MASK (hex, active low: 0080 and 0040 are buttons 1 and 2) low for
// 10 frames; --pedal holds ADC1 (APB's gas pedal) at VALUE; --steer turns the
// LETA0 dial by VALUE counts every frame.
//
// The two CPUs run in slices of main-CPU time (64 cycles by default, 6.4 us),
// the 6502 catching up after the T-11, with the board's timed events
// (scanline and VBLANK interrupts, the 244 Hz sound IRQ, the watchdog) at
// their exact cycles. MAME's T-11 checks its interrupt lines at the start of
// each slice and after instructions that change the PSW, as in MAME.
//
// It reports what the game did (frames, the sound command handshake, sound
// resets, watchdog expiries) and compares the executed code with what the
// static analyses found: every executed location the analysis missed is
// listed with the instruction that led there. That confirms the dynamic
// targets (computed calls and jumps, the banks they run in) and is the
// ground for qilin coverage.
//
// What the analysis missed is turned into seeds: each missed location that
// was entered from analysed code becomes an entry, the analysis runs again
// with them, and that repeats until it covers everything the run executed.
// --seeds-out writes them, one per line ("D758", "4E0C/b12"), for
// qilin cfg and qilin lift --seeds; --seeds starts from an earlier run's, so
// runs with different inputs add up. Needs your ROMs.

#include "cfg.hpp"
#include "m6502_cfg.hpp"
#include "m6502_decoder.hpp"
#include "m6502_reference.h"
#include "romset.hpp"
#include "system2_machine.hpp"
#include "t11_decoder.hpp"
#include "t11_reference.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <charconv>
#include <cstdio>
#include <map>
#include <print>
#include <string_view>
#include <vector>

namespace {

using namespace qilin;

struct MainBus final : t11_device::Bus {
    sys2::Machine& m;
    explicit MainBus(sys2::Machine& machine) : m(machine) {}
    std::uint16_t fetch(std::uint16_t a) override { return m.main_fetch(a); }
    std::uint16_t read_word(std::uint16_t a) override { return m.main_read16(a); }
    void write_word(std::uint16_t a, std::uint16_t d) override { m.main_write16(a, d); }
    std::uint8_t read_byte(std::uint16_t a) override { return m.main_read8(a); }
    void write_byte(std::uint16_t a, std::uint8_t d) override { m.main_write8(a, d); }
};

struct SoundBus final : m6502_device::Bus {
    sys2::Machine& m;
    explicit SoundBus(sys2::Machine& machine) : m(machine) {}
    std::uint8_t read(std::uint16_t a) override { return m.sound_read(a); }
    std::uint8_t read_sync(std::uint16_t a) override { return m.sound_read(a); }
    std::uint8_t read_arg(std::uint16_t a) override { return m.sound_read(a); }
    void write(std::uint16_t a, std::uint8_t v) override { m.sound_write(a, v); }
};

// Where a location was first entered from, for the report.
struct Arrival {
    std::uint16_t from_pc;
    std::uint8_t from_bank;
};

std::string main_name(std::uint16_t pc, std::uint8_t bank) {
    return pc >= 0x8000 || pc < 0x4000 ? std::format("{:04X}", pc) : std::format("{:04X}/b{}", pc, bank);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::print(stderr, "usage: qilin_refrun <rom-dir> [--frames N] [--quantum CYCLES] [--coin FRAME]...\n");
        return 2;
    }
    std::uint64_t frames_wanted = 1800;
    std::int64_t quantum = 64;
    std::vector<std::uint64_t> coins;
    std::vector<std::pair<std::uint16_t, std::uint64_t>> presses;
    int pedal = -1, steer = 0;
    std::string seeds_out;
    std::vector<cfg::Loc> initial_seeds;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const std::string_view value = argv[i + 1];
        std::uint64_t number = 0;
        std::from_chars(value.data(), value.data() + value.size(), number);
        if (flag == "--frames") frames_wanted = number;
        else if (flag == "--quantum") quantum = static_cast<std::int64_t>(std::max<std::uint64_t>(number, 1));
        else if (flag == "--coin") coins.push_back(number);
        else if (flag == "--seeds-out") seeds_out = value;
        else if (flag == "--seeds") {
            const auto loaded = cfg::read_seeds(std::string(value));
            if (!loaded) {
                std::print(stderr, "qilin_refrun: {}\n", loaded.error());
                return 1;
            }
            initial_seeds = *loaded;
        }
        else if (flag == "--pedal") pedal = static_cast<int>(number);
        else if (flag == "--steer") steer = static_cast<int>(number);
        else if (flag == "--press") {
            const auto colon = value.find(':');
            unsigned mask = 0;
            std::uint64_t frame = 0;
            std::from_chars(value.data(), value.data() + colon, mask, 16);
            if (colon != std::string_view::npos) std::from_chars(value.data() + colon + 1, value.data() + value.size(), frame);
            presses.emplace_back(static_cast<std::uint16_t>(mask), frame);
        }
        else {
            std::print(stderr, "qilin_refrun: unknown option {}\n", flag);
            return 2;
        }
    }
    const auto set = rom::RomSet::load(argv[1]);
    if (!set) {
        std::print(stderr, "qilin_refrun: {}\n", set.error());
        return 1;
    }

    sys2::Machine m(set->region(rom::Region::main_cpu), set->region(rom::Region::sound_cpu));
    MainBus main_bus(m);
    SoundBus sound_bus(m);
    t11_device t11(main_bus, 0x36ff);
    t11.reset();
    m6502_device m6502(sound_bus);
    bool sound_was_held = true;

    std::vector<std::uint8_t> main_seen(set->region(rom::Region::main_cpu).size());
    std::vector<std::uint8_t> sound_seen(0x10000);
    std::map<std::uint32_t, Arrival> main_arrivals;   // physical offset -> first arrival
    std::map<std::uint16_t, std::uint16_t> sound_arrivals;
    std::map<std::uint16_t, std::uint64_t> main_ram_code, sound_ram_code;
    std::uint16_t last_main_pc = 0, last_sound_pc = 0;
    std::uint8_t last_main_bank = 0;
    std::uint64_t main_instructions = 0, sound_instructions = 0, watchdog_resets = 0, sound_releases = 0;
    std::uint64_t first_reply_frame = 0, first_command_frame = 0;

    const auto sync_lines = [&] {
        const auto cp = m.main_cp_lines();
        for (int line = 0; line < 4; ++line) t11.set_input_line(line, (cp >> line) & 1);
        m6502.set_input_line(m6502_device::IRQ_LINE, m.sound_irq_line());
        m6502.set_input_line(m6502_device::NMI_LINE, m.sound_nmi_line());
    };

    std::uint64_t reported_second = 0;
    std::uint64_t last_steer_frame = ~0ull;
    while (m.frames() < frames_wanted) {
        // Coins: the left coin switch (IN1 bit 6, active low) closed for 3 frames.
        const bool coin = std::ranges::any_of(coins, [&](std::uint64_t f) { return m.frames() >= f && m.frames() < f + 3; });
        m.inputs.in1 = static_cast<std::uint8_t>(coin ? 0xb0 : 0xf0);
        std::uint16_t in0 = 0xffff;
        for (const auto& [mask, frame] : presses) {
            if (m.frames() >= frame && m.frames() < frame + 10) in0 = static_cast<std::uint16_t>(in0 & ~mask);
        }
        m.inputs.in0 = in0;
        if (pedal >= 0) m.inputs.adc[1] = static_cast<std::uint8_t>(pedal);
        if (steer != 0 && m.frames() != last_steer_frame) {
            last_steer_frame = m.frames();
            m.inputs.leta[0] = static_cast<std::uint8_t>(m.inputs.leta[0] + steer);
        }

        const auto slice_end = std::min(m.next_event(), m.main_time + quantum);
        sync_lines();
        bool first = true;
        while (m.main_time < slice_end) {
            const std::uint8_t banks[2] = {m.bank(0), m.bank(1)};
            const int cycles = t11.step(first);
            first = false;
            if (cycles == 0 && t11.waiting()) {
                m.main_time = slice_end;   // WAIT: idle until an interrupt at a slice boundary
                break;
            }
            const std::uint16_t pc = t11.ppc();
            const std::uint8_t bank = pc >= 0x8000 ? 0 : banks[pc >= 0x6000 ? 1 : 0];
            if (pc < 0x4000) {
                ++main_ram_code[pc];
            } else {
                const auto physical = sys2::rom_offset(pc, banks[0], banks[1]);
                if (!main_seen[physical]) {
                    main_seen[physical] = 1;
                    main_arrivals.emplace(physical, Arrival{last_main_pc, last_main_bank});
                }
            }
            last_main_pc = pc;
            last_main_bank = bank;
            ++main_instructions;
            m.main_time += cycles;
            sync_lines();
        }

        // The sound CPU catches up.
        const auto sound_target = sys2::sound_cycles_at(m.main_time);
        if (m.sound_held_in_reset()) {
            m.sound_time = sound_target;
            sound_was_held = true;
        } else {
            if (sound_was_held) {
                m6502.reset();
                sound_was_held = false;
                ++sound_releases;
            }
            sync_lines();
            while (m.sound_time < sound_target) {
                const bool sequence = m6502.taking_interrupt() || m6502.resetting();
                const std::uint16_t pc = m6502.npc();
                m.sound_time += m6502.step();
                if (!sequence) {
                    if (pc < 0x4000) {
                        ++sound_ram_code[pc];
                    } else if (!sound_seen[pc]) {
                        sound_seen[pc] = 1;
                        sound_arrivals.emplace(pc, last_sound_pc);
                    }
                    last_sound_pc = pc;
                    ++sound_instructions;
                }
                sync_lines();
            }
        }

        m.run_events(m.main_time);
        if (m.take_watchdog_expiry()) {
            ++watchdog_resets;
            m.reset();
            t11.reset();
        }
        if (!first_command_frame && m.counters.sound_commands) first_command_frame = m.frames() + 1;
        if (!first_reply_frame && m.counters.sound_replies) first_reply_frame = m.frames() + 1;
        if (m.main_time / 10'000'000 > static_cast<std::int64_t>(reported_second)) {
            reported_second = static_cast<std::uint64_t>(m.main_time / 10'000'000);
            std::print("  {:3} s: frame {:5}, T-11 at {}, 6502 at {:04X}; commands {} (read {}), replies {} (read {}), "
                       "watchdog resets {}\n",
                       reported_second, m.frames(), main_name(t11.reg(7), t11.reg(7) >= 0x6000 ? m.bank(1) : m.bank(0)),
                       m6502.npc(), m.counters.sound_commands, m.counters.sound_command_reads, m.counters.sound_replies,
                       m.counters.sound_reply_reads, watchdog_resets);
        }
    }

    std::print("\n{} frames ({:.1f} s of game time): {} T-11 and {} 6502 instructions.\n", m.frames(),
               static_cast<double>(m.main_time) / 1e7, main_instructions, sound_instructions);
    std::print("Sound CPU released from reset {} times; first command at frame {}, first reply at frame {}; "
               "{} commands, {} replies; {} EEPROM writes; {} watchdog strobes, {} watchdog resets.\n",
               sound_releases, first_command_frame, first_reply_frame, m.counters.sound_commands, m.counters.sound_replies,
               m.counters.eeprom_writes, m.counters.watchdog_strobes, watchdog_resets);

    // ---- Executed code against the static analyses ----
    const auto loc_of = [](std::uint32_t physical) {
        if (physical < 0x10000) return cfg::Loc{static_cast<std::uint16_t>(physical), 0};
        return cfg::Loc{static_cast<std::uint16_t>(0x4000 + (physical & 0x1fff)), static_cast<std::uint8_t>((physical - 0x10000) / 0x2000)};
    };
    const auto describe_physical = [&](std::uint32_t p) { return loc_of(p).name(); };
    const auto physical_of = [](std::uint16_t pc, std::uint8_t bank) -> std::int64_t {
        if (pc < 0x4000) return -1;
        return pc >= 0x8000 ? pc : 0x10000 + bank * 0x2000 + (pc & 0x1fff);
    };
    std::vector<cfg::Loc> seeds = initial_seeds;
    std::size_t first_missed = 0;
    for (int round = 0;; ++round) {
        const auto program = cfg::Program::analyze(*set, seeds);
        std::vector<std::uint8_t> known(main_seen.size());
        for (const auto& [loc, node] : program.nodes()) known[loc.physical()] = 1;
        std::size_t run = 0, unrun = 0;
        std::vector<std::uint32_t> missed;
        for (std::size_t p = 0; p < main_seen.size(); ++p) {
            if (main_seen[p] && !known[p]) missed.push_back(static_cast<std::uint32_t>(p));
            run += main_seen[p];
            unrun += known[p] && !main_seen[p];
        }
        if (round == 0) {
            first_missed = missed.size();
            std::print("\nT-11: {} locations executed, {} of them not found by the analysis; {} analysed locations not executed.\n",
                       run, missed.size(), unrun);
        }
        // The frontier: missed locations entered from analysed code (or from RAM).
        std::vector<cfg::Loc> frontier;
        for (const auto p : missed) {
            const auto& arrival = main_arrivals.at(p);
            const auto from = physical_of(arrival.from_pc, arrival.from_bank);
            if (from < 0 || known[static_cast<std::size_t>(from)]) {
                frontier.push_back(loc_of(p));
                if (round < 3 && frontier.size() <= 24) {
                    std::print("  round {}: entered {} from {}\n", round + 1, describe_physical(p), main_name(arrival.from_pc, arrival.from_bank));
                }
            }
        }
        if (missed.empty()) {
            std::print("With {} seeds the analysis covers everything the run executed ({} rounds); {} analysed locations "
                       "not executed.\n", seeds.size(), round, unrun);
            break;
        }
        if (frontier.empty() || round == 32) {
            std::print("The analysis still misses {} executed locations after {} seeds.\n", missed.size(), seeds.size());
            break;
        }
        seeds.insert(seeds.end(), frontier.begin(), frontier.end());
    }
    if (!seeds_out.empty()) {
        std::ofstream out(seeds_out, std::ios::trunc);
        out << "# Entries the static analysis can't find, from a reference run of APB rev 7 (qilin_refrun).\n";
        std::ranges::sort(seeds);
        for (const auto& seed : seeds) out << seed.name() << "\n";
        std::print("Wrote {} seeds to {}.\n", seeds.size(), seeds_out);
    }
    for (const auto& [pc, count] : main_ram_code) std::print("  T-11 code in RAM at {:04X}, {} instructions\n", pc, count);
    const std::size_t main_missed = first_missed;

    const auto sound_program = m6502::Program::analyze(set->region(rom::Region::sound_cpu));
    std::size_t sound_run = 0, sound_missed_count = 0, sound_unrun = 0;
    for (unsigned pc = 0x4000; pc < 0x10000; ++pc) {
        const bool known = sound_program.nodes().contains(static_cast<std::uint16_t>(pc));
        if (sound_seen[pc]) {
            ++sound_run;
            if (!known) {
                ++sound_missed_count;
                const auto from = sound_arrivals.at(static_cast<std::uint16_t>(pc));
                if (sound_program.nodes().contains(from) && sound_missed_count <= 40) {
                    std::print("  6502 entered {:04X} from {:04X}\n", pc, from);
                }
            }
        } else if (known) {
            ++sound_unrun;
        }
    }
    std::print("6502: {} locations executed, {} of them not found by the analysis; {} analysed locations not executed.\n",
               sound_run, sound_missed_count, sound_unrun);
    for (const auto& [pc, count] : sound_ram_code) std::print("  6502 code in RAM at {:04X}, {} instructions\n", pc, count);
    return main_missed == 0 && sound_missed_count == 0 ? 0 : 1;
}
