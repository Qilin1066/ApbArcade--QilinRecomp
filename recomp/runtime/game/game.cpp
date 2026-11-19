#include "game.hpp"

#include <format>

#include <algorithm>
#include <cstdio>

namespace qilin::rt {
void Board::service() { scheduler->main_service(); }
void Board::wait() { scheduler->main_wait(); }
Board::Now::Now(Board& board) : b(board), was(board.machine->main_time) {
    b.machine->main_time = b.scheduler->main_now(b.icount);
}

bool Board::interrupt_due() { return scheduler->main_interrupt_due(); }
void Board::check_interrupts() { scheduler->main_check_interrupts(); }
void Board::unlifted() { scheduler->unlifted_main(); }
} // namespace qilin::rt

namespace qilin::rt6502 {
Board::Now::Now(Board& board) : b(board), was(board.machine->sound_time) {
    b.machine->sound_time = b.scheduler->sound_now(b.icount);
}
void Board::service() { scheduler->sound_service(); }
void Board::interrupt_window(int instructions) { scheduler->sound_interrupt_window(instructions); }
void Board::unlifted() { scheduler->unlifted_sound(); }
} // namespace qilin::rt6502

namespace qilin::game {

namespace {

// MAME's T-11 CP interrupt table: the priority and vector for each code on CP0-CP3.
struct IrqEntry {
    std::uint8_t priority;
    std::uint8_t vector;
};
constexpr IrqEntry irq_table[16] = {
    {0 << 5, 0000}, {4 << 5, 0070}, {4 << 5, 0064}, {4 << 5, 0060}, {5 << 5, 0134}, {5 << 5, 0130},
    {5 << 5, 0124}, {5 << 5, 0120}, {6 << 5, 0114}, {6 << 5, 0110}, {6 << 5, 0104}, {6 << 5, 0100},
    {7 << 5, 0154}, {7 << 5, 0150}, {7 << 5, 0144}, {7 << 5, 0140},
};

constexpr std::uint16_t t11_start = 0x8000;   // MAME's initial PC for mode word 0x36FF

std::uint16_t sp_down(std::uint16_t sp) { return static_cast<std::uint16_t>(0x100 | ((sp - 1) & 0xff)); }

char hex_digit(unsigned v) { return "0123456789ABCDEF"[v & 15]; }
std::string hex4(unsigned v) {
    return {hex_digit(v >> 12), hex_digit(v >> 8), hex_digit(v >> 4), hex_digit(v)};
}

} // namespace

Scheduler::Scheduler(sys2::Machine& machine) : m_(machine) {
    main.machine = &m_;
    main.scheduler = this;
    main.bank = m_.bank_registers();
    sound.machine = &m_;
    sound.scheduler = this;
}

Stop Scheduler::run(std::uint64_t frames) {
    stop_frames_ = frames;
    main.cpu = {};
    main.cpu.r[6] = 0376;   // MAME's T-11 reset: SP, PC from the mode word, priority 7
    main.cpu.r[7] = t11_start;
    main.cpu.psw = 0340;
    sound.cpu = {};
    sound.cpu.x = 0x80;     // MAME's 6502 power-on registers
    sound.cpu.sp = 0x0100;
    m_.run_events(m_.main_time);   // those due at power-on, before the first instruction (as MAME's first, empty, slice)
    next_main_slice();
    try {
        for (;;) {
            if (!rt::dispatch(main)) unlifted_main();
        }
    } catch (const Stop& stop) {
        return stop;
    }
}

// ---- Main CPU ---------------------------------------------------------------

void Scheduler::account_main() {
    m_.main_time += main_given_ - main.icount;
    main_given_ = main.icount;
}

void Scheduler::next_main_slice() {
    const auto until_event = m_.next_event() - m_.main_time;
    main.icount = main_given_ = static_cast<std::int32_t>(std::clamp<std::int64_t>(until_event, 1, quantum));
}

void Scheduler::main_service() {
    ++counters.main_services;
    account_main();
    run_sound();
    m_.run_events(m_.main_time);
    if (m_.take_watchdog_expiry()) throw Stop{Stop::watchdog, "the watchdog ran out at frame " + std::to_string(m_.frames())};
    if (m_.frames() >= stop_frames_) throw Stop{Stop::frames, {}};
    if (m_.frames() != frames_seen_) {
        frames_seen_ = m_.frames();
        if (on_frame) on_frame(frames_seen_);
    }
    next_main_slice();
    // MAME checks the lines at the start of each timeslice (and after RTI,
    // RTT and MTPS: main_check_interrupts).
    // A handler that came back here may have used up the slice it was given:
    // the code it interrupted resumes after its own check, so the board's
    // turn comes now, as MAME's slice would end here.
    if (take_main_interrupt(true) && main.icount <= 0) main_service();
}

void Scheduler::main_check_interrupts() { take_main_interrupt(false); }

void Scheduler::main_wait() {
    // WAIT: the T-11 idles, its time passing, until it takes an interrupt.
    // As MAME's core, it idles out each timeslice (at most 'quantum' cycles,
    // ending at the next event), so a line raised between events, by the
    // sound CPU, is seen at the same slice boundary.
    for (;;) {
        account_main();
        if (main.icount > 0) m_.main_time += main.icount;
        main.icount = main_given_ = 0;
        run_sound();
        m_.run_events(m_.main_time);
        if (m_.take_watchdog_expiry()) throw Stop{Stop::watchdog, "the watchdog ran out in WAIT"};
        if (m_.frames() >= stop_frames_) throw Stop{Stop::frames, {}};
        next_main_slice();
        if (take_main_interrupt(true)) return;
    }
}

// MAME's t11_check_irqs and take_interrupt: the vector's PC and PSW are read
// before PSW and PC are pushed; 114 cycles.
bool Scheduler::main_interrupt_due() const {
    return irq_table[m_.main_cp_lines() & 15].priority > (main.cpu.psw & 0340);
}

bool Scheduler::take_main_interrupt(bool at_slice_start) {
    const auto& irq = irq_table[m_.main_cp_lines() & 15];
    if (irq.priority <= (main.cpu.psw & 0340)) return false;
    const std::uint16_t resume = main.cpu.r[7];
    const std::uint16_t new_pc = main.read16(irq.vector);
    const std::uint16_t new_psw = main.read16(static_cast<std::uint16_t>(irq.vector + 2));
    auto& sp = main.cpu.r[6];
    sp = static_cast<std::uint16_t>(sp - 2);
    main.write16(static_cast<std::uint16_t>(sp & 0xfffe), main.cpu.psw);
    sp = static_cast<std::uint16_t>(sp - 2);
    main.write16(static_cast<std::uint16_t>(sp & 0xfffe), resume);
    main.cpu.r[7] = new_pc;
    main.cpu.psw = static_cast<std::uint8_t>(new_psw);
    main.icount -= 114;
    ++counters.main_interrupts;
    // Taken at the start of a slice, MAME's step goes on to run the handler's
    // first instruction in that slice, however far over it already is: the
    // slice is stretched to let it (the accounting shifted with it). Taken
    // after RTI, RTT or MTPS, the step ends there.
    if (at_slice_start && main.icount <= 0) {
        const std::int32_t stretch = 1 - main.icount;
        main.icount += stretch;
        main_given_ += stretch;
    }
    if (!rt::dispatch(main)) unlifted_main();
    // Back from the handler: check again only if it came back here.
    return main.cpu.r[7] == resume;
}

void Scheduler::unlifted_main() {
    // Where it came from is on the stack, usually: the registers and the
    // top of the stack go in the report.
    std::string regs;
    for (int i = 0; i < 7; ++i) regs += std::format(" R{}={:04X}", i, main.cpu.r[i]);
    std::string stack;
    for (int i = 0; i < 4; ++i) stack += " " + hex4(m_.main_read16(static_cast<std::uint16_t>(main.cpu.r[6] + 2 * i)));
    throw Stop{Stop::unlifted_main, "no lifted T-11 code at " + hex4(main.cpu.r[7]) + " (banks " + std::to_string(main.bank[0]) +
                                        ", " + std::to_string(main.bank[1]) + ");" + regs + "; stack" + stack};
}

// ---- Sound CPU --------------------------------------------------------------

void Scheduler::account_sound() {
    m_.sound_time += sound_given_ - sound.icount;
    sound_given_ = sound.icount;
}

void Scheduler::run_sound() {
    const auto target = sys2::sound_cycles_at(m_.main_time);
    if (m_.sound_held_in_reset()) {
        m_.sound_time = std::max(m_.sound_time, target);
        sound_held_ = true;
        return;
    }
    if (sound_held_) {
        // Out of reset: MAME's reset sequence, 8 cycles, ending with the first opcode fetch.
        sound_held_ = false;
        nmi_pending_ = false;                        // MAME's reset clears a pending NMI
        nmi_edges_seen_ = m_.sound_nmi_edges();
        ++counters.sound_resets;
        auto& c = sound.cpu;
        sound.read_arg(c.pc);
        sound.read_arg(c.pc);
        for (int i = 0; i < 3; ++i) {
            sound.read(c.sp);
            c.sp = sp_down(c.sp);
        }
        c.p |= rt6502::F_I;
        c.pc = static_cast<std::uint16_t>(sound.read_arg(0xfffc) | (sound.read_arg(0xfffd) << 8));
        sound.read_sync(c.pc);
        m_.sound_time += 8;
    }
    sound_target_ = target;
    while (m_.sound_time < sound_target_) {
        sound.icount = sound_given_ = static_cast<std::int32_t>(sound_target_ - m_.sound_time);
        sound.yielding = false;
        // Interrupts at the start of the slice: NMI on the latch's rising edge, IRQ while unmasked.
        if (m_.sound_nmi_edges() != nmi_edges_seen_) {
            nmi_edges_seen_ = m_.sound_nmi_edges();
            nmi_pending_ = true;
        }
        // MAME samples the lines during an instruction and decides at its
        // end: a line raised here is taken after one more instruction.
        if (decision_ == Decision::now || (decision_ == Decision::next && m_.sound_time > decide_after_)) {
            sound.icount = sound_given_ = 0;   // one still to make from the last slice: at once
        } else if (decision_ == Decision::next) {
            sound.icount = sound_given_ = 1;
        } else if (nmi_pending_ || sound_irq_ok()) {
            sound_open_decision(Decision::next);
        }
        if (!rt6502::dispatch(sound)) unlifted_sound();
        account_sound();
    }
    sound_end_slice();
}

// MAME's brk_imp with irq_taken: a read at PC, PC and P (B clear) pushed,
// the vector read, I set, the handler's first opcode fetched. 7 cycles.
void Scheduler::take_sound_interrupt(bool nmi) {
    auto& c = sound.cpu;
    sound.read_arg(c.pc);
    sound.write(c.sp, static_cast<std::uint8_t>(c.pc >> 8));
    c.sp = sp_down(c.sp);
    sound.write(c.sp, static_cast<std::uint8_t>(c.pc));
    c.sp = sp_down(c.sp);
    sound.write(c.sp, static_cast<std::uint8_t>(c.p & ~rt6502::F_B));
    c.sp = sp_down(c.sp);
    const std::uint16_t vector = nmi ? 0xfffa : 0xfffe;
    c.pc = static_cast<std::uint16_t>(sound.read_arg(vector) | (sound.read_arg(static_cast<std::uint16_t>(vector + 1)) << 8));
    c.p |= rt6502::F_I;
    sound.read_sync(c.pc);
    sound.icount -= 7;
    ++(nmi ? counters.sound_nmis : counters.sound_irqs);
}

// MAME's 6502 decides on an interrupt at the end of each instruction, from
// the lines it sampled during it and the I flag as it stands before CLI, PLP
// or SEI change it (after RTI, as RTI left it). The lines change only between
// slices here, so a decision is due:
//   'next': after the next instruction, on the lines and I as they are then
//           (a line raised at a slice start; after CLI or PLP, the new I);
//   'now':  before the next instruction, on what was sampled (before CLI,
//           PLP or SEI, the old I; after RTI, the new; a 'next' whose
//           instruction ran before its slice ended).
// Lifted code checks its cycles before every instruction (lifted with
// --service-interval 0), so 'next' gives it one cycle and 'now' none.
void Scheduler::sound_open_decision(Decision kind) {
    decision_ = kind;
    decide_after_ = m_.sound_time;
    const auto left = std::max<std::int64_t>(sound_target_ - m_.sound_time, 0);
    sound.icount = sound_given_ = static_cast<std::int32_t>(std::min<std::int64_t>(left, kind == Decision::next ? 1 : 0));
}

bool Scheduler::sound_irq_ok() const { return m_.sound_irq_line() && !(sound.cpu.p & rt6502::F_I); }

// A 'next' whose instruction has run goes by the lines as they were during
// it, not as the board's events leave them for the next slice.
void Scheduler::sound_end_slice() {
    if (decision_ == Decision::next && m_.sound_time > decide_after_) {
        sampled_nmi_ = nmi_pending_;
        sampled_irq_ok_ = sound_irq_ok();
        decision_ = Decision::now;
    }
}

void Scheduler::sound_interrupt_window(int when) {
    account_sound();
    if (when < 0) {
        // Before CLI, PLP or SEI: the decision at its end goes by the I flag
        // as it is now (they change it after their last fetch).
        if (nmi_pending_ || sound_irq_ok()) {
            sampled_nmi_ = nmi_pending_;
            sampled_irq_ok_ = sound_irq_ok();
            sound_open_decision(Decision::now);
        } else if (decision_ == Decision::next) {
            decision_ = Decision::none;   // the decision it waited for: nothing
        }
    } else if (when == 0) {
        // After RTI: on the I flag it restored.
        sampled_nmi_ = nmi_pending_;
        sampled_irq_ok_ = sound_irq_ok();
        sound_open_decision(Decision::now);
    } else if (decision_ == Decision::none && (nmi_pending_ || sound_irq_ok())) {
        // After CLI or PLP: the new I counts from the next instruction's end.
        sound_open_decision(Decision::next);
    }
}

void Scheduler::sound_service() {
    account_sound();
    // Decide in the slice: an interrupt sequence is a step of its own on MAME's core.
    const bool due = decision_ == Decision::now || (decision_ == Decision::next && m_.sound_time > decide_after_);
    if (due && m_.sound_time < sound_target_) {
        const bool now = decision_ == Decision::now;
        decision_ = Decision::none;
        const bool nmi = now ? sampled_nmi_ : nmi_pending_;
        const bool irq_ok = now ? sampled_irq_ok_ : sound_irq_ok();
        if (nmi || irq_ok) {
            // The sequence picks its vector as it runs: an NMI pending by
            // then wins, even over the IRQ that started it (MAME's brk_imp).
            const bool use_nmi = nmi_pending_;
            nmi_pending_ = false;
            take_sound_interrupt(use_nmi);
            account_sound();
        } else if (nmi_pending_ || sound_irq_ok()) {
            // Raised since that sample, or unmasked by it: after the next instruction.
            sound_open_decision(Decision::next);
            return;
        }
    }
    if (m_.sound_time >= sound_target_) {
        sound_end_slice();
        sound.yielding = true;   // the slice is over: the lifted code unwinds, run_sound resumes it later
        return;
    }
    sound.icount = sound_given_ = static_cast<std::int32_t>(sound_target_ - m_.sound_time);
}

void Scheduler::unlifted_sound() {
    throw Stop{Stop::unlifted_sound, "no lifted 6502 code at " + hex4(sound.cpu.pc)};
}

} // namespace qilin::game
