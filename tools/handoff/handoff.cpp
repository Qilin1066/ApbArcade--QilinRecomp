// Hands a native run over to the reference cores: the game runs natively
// (both CPUs' lifted code) to a frame, then the board, the T-11's and the
// 6502's state are copied to MAME's cores, which carry on with the same
// inputs. Where the native run does something the reference doesn't (stops,
// runs code in RAM, takes the stack deeper), this says whether the native
// state itself leads there on MAME's cores, or the native code went wrong
// after it.
//
//   qilin_handoff <rom-dir> --at FRAME [--frames N] [--quantum CYCLES] and qilin_run's controls (--coin, --press, --pedal, --steer, --monkey, --test-switch)
//
// Reports, per frame of the reference run, the lowest T-11 stack pointer and
// any T-11 execution outside ROM. Needs your ROMs and both lifts, like qilin_run.

#include "game.hpp"
#include "m6502_reference.h"
#include "script.hpp"
#include "romset.hpp"
#include "system2_machine.hpp"
#include "t11_reference.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <print>
#include <string_view>

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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::print(stderr, "usage: qilin_handoff <rom-dir> --at FRAME [--frames N] [--monkey SEED] [--test-switch 1]\n");
        return 2;
    }
    std::uint64_t at = 0, frames = 300;
    std::int64_t quantum = 64;
    tools::Script script;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i], value = argv[i + 1];
        std::uint64_t number = 0;
        std::from_chars(value.data(), value.data() + value.size(), number);
        if (flag == "--at") at = number;
        else if (flag == "--frames") frames = number;
        else if (flag == "--quantum") quantum = static_cast<std::int64_t>(number);
        else if (script.option(flag, value)) {}
        else {
            std::print(stderr, "qilin_handoff: unknown option {}\n", flag);
            return 2;
        }
    }
    const auto set = rom::RomSet::load(argv[1]);
    if (!set) {
        std::print(stderr, "qilin_handoff: {}\n", set.error());
        return 1;
    }

    // ---- Natively, to the frame ----
    sys2::Machine native(set->region(rom::Region::main_cpu), set->region(rom::Region::sound_cpu));
    script.attach(native);
    game::Scheduler scheduler(native);
    const auto stop = scheduler.run(at);
    if (stop.reason != game::Stop::frames) {
        std::print("The native run stopped before frame {}: {}\n", at, stop.detail);
        return 1;
    }
    std::print("Native at frame {}: T-11 at {:04X} (banks {}/{}), SP {:04X}, PSW {:03o}; 6502 at {:04X}.\n", native.frames(),
               scheduler.main.cpu.r[7], native.bank(0), native.bank(1), scheduler.main.cpu.r[6], scheduler.main.cpu.psw,
               scheduler.sound.cpu.pc);

    // ---- The same state on MAME's cores ----
    sys2::Machine m = native;
    m.video_sink = nullptr;
    m.sound_sink = nullptr;
    script.drive(m);
    MainBus main_bus(m);
    SoundBus sound_bus(m);
    t11_device t11(main_bus, 0x36ff);
    t11.reset();
    for (int r = 0; r < 8; ++r) t11.set_reg(r, scheduler.main.cpu.r[r]);
    t11.set_psw(scheduler.main.cpu.psw);
    m6502_device m6502(sound_bus);
    m6502.reset();
    const auto& s = scheduler.sound.cpu;
    m6502.set_state(s.pc, s.a, s.x, s.y, s.p, static_cast<std::uint8_t>(s.sp & 0xff));
    const auto sync_lines = [&] {
        const auto cp = m.main_cp_lines();
        for (int line = 0; line < 4; ++line) t11.set_input_line(line, (cp >> line) & 1);
        m6502.set_input_line(m6502_device::IRQ_LINE, m.sound_irq_line());
        m6502.set_input_line(m6502_device::NMI_LINE, m.sound_nmi_line());
    };

    std::uint16_t lowest = 0xffff;
    std::uint64_t last_frame = m.frames(), outside = 0;
    std::uint16_t first_outside = 0;
    const std::uint64_t end = at + frames;
    bool sound_held = m.sound_held_in_reset();
    while (m.frames() < end) {
        if (m.frames() != last_frame) {
            std::print("  frame {}: lowest SP {:04X}{}\n", last_frame, lowest,
                       outside ? std::format(", {} instructions outside ROM from {:04X}", outside, first_outside) : "");
            last_frame = m.frames();
            lowest = 0xffff;
            outside = 0;
        }
        const auto slice_end = std::min(m.next_event(), m.main_time + quantum);
        sync_lines();
        bool first = true;
        while (m.main_time < slice_end) {
            const int cycles = t11.step(first);
            first = false;
            if (cycles == 0 && t11.waiting()) {
                m.main_time = slice_end;
                break;
            }
            const auto pc = t11.ppc();
            if (pc < 0x4000 && outside++ == 0) first_outside = pc;
            lowest = std::min<std::uint16_t>(lowest, static_cast<std::uint16_t>(t11.reg(6)));
            m.main_time += cycles;
            sync_lines();
        }
        const auto sound_target = sys2::sound_cycles_at(m.main_time);
        if (m.sound_held_in_reset()) {
            m.sound_time = sound_target;
            sound_held = true;
        } else {
            if (sound_held) {
                m6502.reset();
                sound_held = false;
            }
            sync_lines();
            while (m.sound_time < sound_target) {
                m.sound_time += m6502.step();
                sync_lines();
            }
        }
        m.run_events(m.main_time);
        if (m.take_watchdog_expiry()) {
            std::print("  frame {}: the watchdog ran out\n", m.frames());
            m.reset();
            t11.reset();
        }
    }
    return 0;
}
