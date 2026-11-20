// Reference run: the game on MAME's own CPU cores (third_party/mame-t11 and
// third_party/mame-m6502) and this project's System 2 board model
// (recomp/runtime/system2_machine.*), with every executed address recorded.
//
//   qilin_refrun <rom-dir> [--frames N] [--quantum CYCLES] [--coin FRAME]... [--press MASK:FRAME]...
//                [--pedal VALUE] [--steer VALUE] [--seeds FILE] [--seeds-out FILE]
//                [--frame-out FRAME:FILE]... [--frame-hashes FILE] [--monkey SEED] [--test-switch 1] [--executed-out FILE] [--reads-out FILE]
//
// --coin closes the left coin switch for 3 frames; --press pulls the IN0
// bits in MASK (hex, active low: 0008 siren/start, 0002 fire) low for
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
#include "frame_capture.hpp"
#include "script.hpp"
#include "t11_decoder.hpp"
#include "t11_reference.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <charconv>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <print>
#include <string_view>
#include <vector>

namespace {

using namespace qilin;

struct MainBus final : t11_device::Bus {
    sys2::Machine& m;
    bool note_reads = false;
    const t11_device* cpu = nullptr;   // set once the core exists
    // I/O sees MAME's local time: the instruction's start plus the cycles it has charged.
    struct Now {
        MainBus& bus;
        std::int64_t was;
        explicit Now(MainBus& b) : bus(b), was(b.m.main_time) {
            if (bus.cpu) bus.m.main_time = was - bus.cpu->icount();
        }
        ~Now() { bus.m.main_time = was; }
    };
    std::vector<std::uint32_t> pending;   // ROM words (physical) the current instruction read as data
    explicit MainBus(sys2::Machine& machine) : m(machine) {}
    void note(std::uint16_t a) {
        if (note_reads && a >= 0x4000) pending.push_back(sys2::rom_offset(a, m.bank(0), m.bank(1)) / 2);
    }
    std::uint16_t fetch(std::uint16_t a) override { return m.main_fetch(a); }
    std::uint16_t read_word(std::uint16_t a) override { note(a); Now now(*this); return m.main_read16(a); }
    void write_word(std::uint16_t a, std::uint16_t d) override { Now now(*this); m.main_write16(a, d); }
    std::uint8_t read_byte(std::uint16_t a) override { note(a); Now now(*this); return m.main_read8(a); }
    void write_byte(std::uint16_t a, std::uint8_t d) override { Now now(*this); m.main_write8(a, d); }
};

struct SoundBus final : m6502_device::Bus {
    sys2::Machine& m;
    const m6502_device* cpu = nullptr;   // set once the core exists
    // The chips see MAME's local time: the instruction's start plus the bus cycles before this one.
    struct Now {
        SoundBus& bus;
        std::int64_t was;
        explicit Now(SoundBus& b) : bus(b), was(b.m.sound_time) {
            if (bus.cpu) bus.m.sound_time = was + bus.cpu->elapsed();
        }
        ~Now() { bus.m.sound_time = was; }
    };
    explicit SoundBus(sys2::Machine& machine) : m(machine) {}
    std::uint8_t read(std::uint16_t a) override { Now now(*this); return m.sound_read(a); }
    std::uint8_t read_sync(std::uint16_t a) override { Now now(*this); return m.sound_read(a); }
    std::uint8_t read_arg(std::uint16_t a) override { Now now(*this); return m.sound_read(a); }
    void write(std::uint16_t a, std::uint8_t v) override { Now now(*this); m.sound_write(a, v); }
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
    if (argc % 2 != 0) {
        std::fprintf(stderr, "Each option needs a value\n"); return 2;
    }
    std::uint64_t frames_wanted = 1800;
    std::int64_t quantum = 64;
    tools::Script script;
    std::string seeds_out, log_path, executed_out, reads_out;
    std::vector<cfg::Loc> initial_seeds;
    tools::FrameCapture capture;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const std::string_view value = argv[i + 1];
        std::uint64_t number = 0;
        std::from_chars(value.data(), value.data() + value.size(), number);
        if (flag == "--frames") frames_wanted = number;
        else if (flag == "--quantum") quantum = static_cast<std::int64_t>(std::max<std::uint64_t>(number, 1));
        else if (script.option(flag, value)) {}
        else if (flag == "--frame-hashes") capture.hashes_path = value;
        else if (flag == "--seeds-out") seeds_out = value;
        else if (flag == "--executed-out") executed_out = value;
        else if (flag == "--reads-out") reads_out = value;
        else if (flag == "--log") log_path = value;
        else if (flag == "--frame-out") {
            if (!capture.add(value)) {
                std::print(stderr, "qilin_refrun: --frame-out wants FRAME:FILE\n");
                return 2;
            }
        }
        else if (flag == "--seeds") {
            const auto loaded = cfg::read_seeds(std::string(value));
            if (!loaded) {
                std::print(stderr, "qilin_refrun: {}\n", loaded.error());
                return 1;
            }
            initial_seeds = *loaded;
        }
        else {
            std::print(stderr, "qilin_refrun: unknown option {}\n", flag);
            return 2;
        }
    }
    if (!script.error().empty()) {
        std::fprintf(stderr, "%s\n", script.error().c_str()); return 2;
    }
    const auto set = rom::RomSet::load(argv[1]);
    if (!set) {
        std::print(stderr, "qilin_refrun: {}\n", set.error());
        return 1;
    }

    sys2::Machine m(set->region(rom::Region::main_cpu), set->region(rom::Region::sound_cpu));
    std::vector<sys2::Machine::LogEntry> log;
    if (!log_path.empty()) m.log = &log;
    MainBus main_bus(m);
    // Who read each ROM word as data: (word, physical address of the reading instruction).
    std::set<std::pair<std::uint32_t, std::uint32_t>> data_reads;
    if (!reads_out.empty()) main_bus.note_reads = true;
    SoundBus sound_bus(m);
    t11_device t11(main_bus, 0x36ff);
    main_bus.cpu = &t11;
    t11.reset();
    m6502_device m6502(sound_bus);
    bool sound_was_held = true;

    std::vector<std::uint8_t> main_seen(set->region(rom::Region::main_cpu).size());
    std::vector<std::uint8_t> sound_seen(0x10000);
    std::map<std::uint32_t, Arrival> main_arrivals;   // physical offset -> first arrival
    std::map<std::uint16_t, std::uint16_t> sound_arrivals;
    std::map<std::uint16_t, std::uint64_t> main_ram_code, sound_ram_code;
    std::uint16_t last_main_pc = 0, last_sound_pc = 0;
    std::set<std::pair<std::uint32_t, std::uint32_t>> transfers;   // physical from, to
    const auto physical_of = [](std::uint16_t pc, std::uint8_t bank) -> std::int64_t {
        if (pc < 0x4000) return -1;
        return pc >= 0x8000 ? pc : 0x10000 + bank * 0x2000 + (pc & 0x1fff);
    };
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
    if (!capture.attach(*set, m)) return 1;
    script.attach(m);   // the controls change at each vblank, on the cycle qilin_run changes them
    while (m.frames() < frames_wanted) {
        capture.poll(m);
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
            if (!main_bus.pending.empty()) {
                const auto reader = pc >= 0x4000 ? sys2::rom_offset(pc, banks[0], banks[1]) : 0xffffffffu;
                for (const auto w : main_bus.pending) data_reads.emplace(w, reader);
                main_bus.pending.clear();
            }
            // Transfers that aren't to the next instruction (it is at most 6
            // bytes on): computed ones among them must land on lifted entries.
            if (pc >= 0x4000 && last_main_pc >= 0x4000 && (pc <= last_main_pc || pc > last_main_pc + 6)) {
                const auto from = physical_of(last_main_pc, last_main_bank);
                transfers.emplace(static_cast<std::uint32_t>(from), sys2::rom_offset(pc, banks[0], banks[1]));
            }
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
                sound_bus.cpu = &m6502;   // only within a step: its bus cycles are counted from the step's start
                m.sound_time += m6502.step();
                sound_bus.cpu = nullptr;
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

    if (!log_path.empty()) {
        std::ofstream out(log_path, std::ios::trunc);
        for (const auto& e : log) out << std::format("{} {:02X} {}\n", e.what, e.value, e.time);
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
            // Everything executed is analysed; a computed jump or call into the
            // middle of analysed code also needs its target to be an entry
            // lifted code can be dispatched to (a function or a return site:
            // coroutine calls return through CALL @(SP)+).
            std::map<std::uint32_t, cfg::Loc> by_physical;
            for (const auto& [loc, node] : program.nodes()) by_physical.emplace(loc.physical(), loc);
            std::vector<cfg::Loc> entries;
            for (const auto& [from, to] : transfers) {
                const auto f = by_physical.find(from), t = by_physical.find(to);
                if (f == by_physical.end() || t == by_physical.end()) continue;
                const auto flow = program.nodes().at(f->second).flow;
                if ((flow == cfg::Flow::jump_dynamic || flow == cfg::Flow::call_dynamic) && !program.functions().contains(t->second) &&
                    !program.return_sites().contains(t->second) &&
                    std::ranges::find(seeds, t->second) == seeds.end() && std::ranges::find(entries, t->second) == entries.end()) {
                    entries.push_back(t->second);
                    std::print("  entry {} from the computed transfer at {}\n", t->second.name(), f->second.name());
                }
            }
            if (!entries.empty()) {
                seeds.insert(seeds.end(), entries.begin(), entries.end());
                continue;
            }
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
    if (!executed_out.empty()) {
        // Every T-11 location executed, added to what the file already lists
        // (so runs add up), in the seed file format: qilin coverage --executed.
        std::set<cfg::Loc> executed;
        if (const auto earlier = cfg::read_seeds(executed_out); earlier) executed.insert(earlier->begin(), earlier->end());
        for (std::size_t p = 0; p < main_seen.size(); ++p)
            if (main_seen[p]) executed.insert(loc_of(static_cast<std::uint32_t>(p)));
        std::ofstream out(executed_out, std::ios::trunc);
        for (const auto& loc : executed) out << loc.name() << '\n';
        std::print("{} executed T-11 locations in {}.\n", executed.size(), executed_out);
    }
    if (!reads_out.empty()) {
        // ROM locations the T-11 read as data, added to what the file lists: qilin coverage --reads.
        std::set<cfg::Loc> reads;
        if (const auto earlier = cfg::read_seeds(reads_out); earlier) reads.insert(earlier->begin(), earlier->end());
        // A reader that sweeps a whole ROM (the checksums) is no
        // evidence of what any one of them is: leave its reads out.
        std::map<std::uint32_t, std::size_t> per_reader;
        for (const auto& [w, reader] : data_reads) ++per_reader[reader];
        for (const auto& [w, reader] : data_reads) {
            if (per_reader[reader] > 8192) continue;
            if (w * 2 >= 0x8000) reads.insert(loc_of(w * 2));
        }
        for (const auto& [reader, n] : per_reader) if (n > 8192) {
            std::print("Left out the {} words read by the sweep at {}.\n", n, describe_physical(reader));
        }
        std::ofstream out(reads_out, std::ios::trunc);
        for (const auto& loc : reads) out << loc.name() << '\n';
        std::print("{} ROM words read as data in {}.\n", reads.size(), reads_out);
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
