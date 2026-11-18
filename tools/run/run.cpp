// Headless native run: both CPUs' lifted code on the System 2 board model,
// scheduled by recomp/runtime/game, from power-on for a number of frames.
//
//   qilin_run <rom-dir> [--frames N] [--coin FRAME]... [--press MASK:FRAME]... [--pedal V] [--steer V] [--log FILE]
//             [--frame-out FRAME:FILE]...
//
// The inputs are qilin_refrun's, so runs of the two can be compared.
//
// Reports how far the game got (frames, the sound handshake, interrupts
// taken, sound resets) and why it stopped: the frame count, the watchdog, or
// control reaching an address nothing was lifted for. Needs your ROMs and a
// build with both -DQILIN_GEN_DIR (qilin lift) and -DQILIN_SOUND_GEN_DIR
// (qilin lift --cpu sound).

#include "frame_capture.hpp"
#include "game.hpp"
#include "romset.hpp"
#include "system2_machine.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <string>
#include <vector>
#include <charconv>
#include <print>
#include <string_view>

int main(int argc, char** argv) {
    using namespace qilin;
    if (argc < 2) {
        std::print(stderr, "usage: qilin_run <rom-dir> [--frames N]\n");
        return 2;
    }
    std::uint64_t frames = 600;
    std::string_view log_path;
    std::vector<std::uint64_t> coins;
    std::vector<std::pair<std::uint16_t, std::uint64_t>> presses;
    int pedal = -1, steer = 0;
    tools::FrameCapture capture;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i], value = argv[i + 1];
        std::uint64_t number = 0;
        std::from_chars(value.data(), value.data() + value.size(), number);
        if (flag == "--frames") frames = number;
        else if (flag == "--log") log_path = value;
        else if (flag == "--frame-out") {
            if (!capture.add(value)) {
                std::print(stderr, "qilin_run: --frame-out wants FRAME:FILE\n");
                return 2;
            }
        }
        else if (flag == "--coin") coins.push_back(number);
        else if (flag == "--pedal") pedal = static_cast<int>(number);
        else if (flag == "--steer") steer = static_cast<int>(number);
        else if (flag == "--press") {
            const auto colon = value.find(':');
            unsigned mask = 0;
            std::uint64_t frame = 0;
            std::from_chars(value.data(), value.data() + colon, mask, 16);
            if (colon != std::string_view::npos) std::from_chars(value.data() + colon + 1, value.data() + value.size(), frame);
            presses.emplace_back(static_cast<std::uint16_t>(mask), frame);
        } else {
            std::print(stderr, "qilin_run: unknown option {}\n", flag);
            return 2;
        }
    }
    const auto set = rom::RomSet::load(argv[1]);
    if (!set) {
        std::print(stderr, "qilin_run: {}\n", set.error());
        return 1;
    }
    sys2::Machine machine(set->region(rom::Region::main_cpu), set->region(rom::Region::sound_cpu));
    std::vector<sys2::Machine::LogEntry> log;
    if (!log_path.empty()) machine.log = &log;
    capture.attach(*set, machine);
    game::Scheduler scheduler(machine);
    // The same inputs as qilin_refrun: coins for 3 frames, presses for 10.
    scheduler.on_frame = [&](std::uint64_t frame) {
        capture.poll(machine);
        const bool coin = std::ranges::any_of(coins, [&](std::uint64_t f) { return frame >= f && frame < f + 3; });
        machine.inputs.in1 = static_cast<std::uint8_t>(coin ? 0xb0 : 0xf0);
        std::uint16_t in0 = 0xffff;
        for (const auto& [mask, at] : presses) {
            if (frame >= at && frame < at + 10) in0 = static_cast<std::uint16_t>(in0 & ~mask);
        }
        machine.inputs.in0 = in0;
        if (pedal >= 0) machine.inputs.adc[1] = static_cast<std::uint8_t>(pedal);
        if (steer != 0) machine.inputs.leta[0] = static_cast<std::uint8_t>(machine.inputs.leta[0] + steer);
    };
    const auto start = std::chrono::steady_clock::now();
    const auto stop = scheduler.run(frames);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    const char* reasons[] = {"the frame count", "the watchdog", "unlifted main-CPU code", "unlifted sound-CPU code"};
    std::print("Stopped by {}{}{} at frame {} ({:.2f} s of game time, {:.2f} s here: {:.1f}x real time).\n",
               reasons[stop.reason], stop.detail.empty() ? "" : ": ", stop.detail, machine.frames(),
               static_cast<double>(machine.main_time) / 1e7, seconds, static_cast<double>(machine.main_time) / 1e7 / seconds);
    const auto& c = machine.counters;
    const auto& s = scheduler.counters;
    std::print("Sound: released from reset {} times; {} commands ({} read), {} replies ({} read); {} NMIs, {} IRQs.\n",
               s.sound_resets, c.sound_commands, c.sound_command_reads, c.sound_replies, c.sound_reply_reads, s.sound_nmis,
               s.sound_irqs);
    std::print("Main: {} interrupts taken, {} services; {} watchdog strobes; {} EEPROM writes.\n", s.main_interrupts,
               s.main_services, c.watchdog_strobes, c.eeprom_writes);
    std::print("T-11 at {:04X}, PSW {:03o}, banks {}/{}; CP lines {:X}, interrupt enable {:X}; 6502 at {:04X}.\n",
               scheduler.main.cpu.r[7], scheduler.main.cpu.psw, machine.bank(0), machine.bank(1), machine.main_cp_lines(),
               machine.interrupt_enable(), scheduler.sound.cpu.pc);
    if (!log_path.empty()) {
        std::ofstream out{std::string(log_path), std::ios::trunc};
        for (const auto& e : log) out << std::format("{} {:02X} {}\n", e.what, e.value, e.time);
    }
    return stop.reason == game::Stop::frames ? 0 : 1;
}
