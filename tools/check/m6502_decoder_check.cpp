// Cross-checks the 6502 decoder against MAME, which is this project's reference.
//
// For all 256 opcodes:
//   - the decoder's disassembly must equal MAME's disassembler (m6502d.cpp)
//     at several PCs and with several operand bytes, and its length and
//     control flow must agree with MAME's length and step flags;
//   - running the opcode in MAME's 6502 core (m6502.hxx) must take the
//     decoder's cycle count, with and without a page crossing, taken and not
//     taken for branches, make one bus access per cycle, and leave the PC
//     where the decoder's length and flow say it goes.
// Also checks the IRQ, NMI and reset sequences against interrupt_cycles,
// reset_cycles and the vectors.
//
// Needs no game files. Exit status 1 on any disagreement.

#include "m6502_decoder.hpp"

#include "emu.h"
#include "m6502_reference.h"
#include "m6502d.h"

#include <array>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace m6502 = qilin::m6502;
using m6502::Flow;
using m6502::Mode;

struct Ram final : m6502_device::Bus {
    std::array<std::uint8_t, 0x10000> bytes{};
    int accesses = 0;

    std::uint8_t read(std::uint16_t adr) override { ++accesses; return bytes[adr]; }
    std::uint8_t read_sync(std::uint16_t adr) override { ++accesses; return bytes[adr]; }
    std::uint8_t read_arg(std::uint16_t adr) override { ++accesses; return bytes[adr]; }
    void write(std::uint16_t adr, std::uint8_t val) override { ++accesses; bytes[adr] = val; }

    void put16(std::uint16_t adr, std::uint16_t value) {
        bytes[adr] = static_cast<std::uint8_t>(value);
        bytes[static_cast<std::uint16_t>(adr + 1)] = static_cast<std::uint8_t>(value >> 8);
    }
    std::uint16_t get16(std::uint16_t adr) const {
        return static_cast<std::uint16_t>(bytes[adr] | (bytes[static_cast<std::uint16_t>(adr + 1)] << 8));
    }
};

constexpr std::uint16_t irq_handler = 0x0400;
constexpr std::uint16_t nmi_handler = 0x0500;
constexpr std::uint16_t reset_handler = 0x0300;
constexpr std::uint8_t stack = 0xf0;
constexpr std::uint16_t pushed_word = 0x0100 + stack - 1;   // where a 2-byte push from S = stack lands
constexpr std::uint16_t pushed_p = 0x0100 + stack - 2;      // and the P an interrupt pushes after it

int failures = 0;
int runs = 0;

void fail(const std::string& message) {
    if (++failures <= 40) {
        std::fprintf(stderr, "%s\n", message.c_str());
    }
}

std::string hex(unsigned value, int digits = 2) {
    char text[16];
    std::snprintf(text, sizeof text, "%0*x", digits, value);
    return text;
}

void check_disassembly() {
    m6502_disassembler mame;
    std::array<std::uint8_t, 0x10000> memory{};
    constexpr std::uint16_t pcs[] = {0x0000, 0x0280, 0x80fe, 0xfff0};
    constexpr std::uint8_t operands[][2] = {{0x00, 0x00}, {0x34, 0x12}, {0x80, 0xff}, {0x7f, 0x00}, {0xfe, 0x80}};
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        for (const std::uint16_t pc : pcs) {
            for (const auto& operand : operands) {
                memory[pc] = static_cast<std::uint8_t>(opcode);
                memory[static_cast<std::uint16_t>(pc + 1)] = operand[0];
                memory[static_cast<std::uint16_t>(pc + 2)] = operand[1];
                const util::disasm_interface::data_buffer buffer(memory.data(), 0);
                std::ostringstream text;
                const offs_t flags = mame.disassemble(text, pc, buffer, buffer);
                const auto insn = m6502::decode(static_cast<std::uint8_t>(opcode), operand[0], operand[1]);
                const std::string ours = m6502::disassemble(insn, pc);
                const std::string where = hex(opcode) + " at " + hex(pc, 4);
                if (ours != text.str()) {
                    fail(where + ": disassembly '" + ours + "', MAME '" + text.str() + "'");
                }
                if (insn.length != (flags & util::disasm_interface::LENGTHMASK)) {
                    fail(where + ": length " + std::to_string(insn.length) + ", MAME " +
                         std::to_string(flags & util::disasm_interface::LENGTHMASK));
                }
                const bool cond = (flags & util::disasm_interface::STEP_COND) != 0;
                const bool over = (flags & util::disasm_interface::STEP_OVER) != 0;
                const bool out = (flags & util::disasm_interface::STEP_OUT) != 0;
                if (cond != (insn.flow == Flow::branch) || over != (insn.flow == Flow::call) ||
                    out != (insn.flow == Flow::ret || insn.flow == Flow::rti)) {
                    fail(where + ": control flow disagrees with MAME's step flags");
                }
            }
        }
    }
}

struct Run {
    int cycles = 0;
    int accesses = 0;
    std::uint16_t npc = 0;
};

// One instruction at pc in MAME's core, with the given registers.
Run execute(Ram& ram, std::uint16_t pc, std::uint8_t x, std::uint8_t y, std::uint8_t p) {
    ram.put16(m6502::irq_vector, irq_handler);
    ram.put16(m6502::nmi_vector, nmi_handler);
    ram.put16(m6502::reset_vector, reset_handler);
    m6502_device cpu(ram);
    cpu.set_state(pc, 0x5a, x, y, static_cast<std::uint8_t>(p | 0x30), stack);
    ram.accesses = 0;
    Run run;
    run.cycles = cpu.step();
    run.accesses = ram.accesses;
    run.npc = cpu.npc();
    ++runs;
    return run;
}

// The scenarios each addressing mode runs in: index registers, and whether
// the effective address crosses a page.
struct Scenario {
    std::uint8_t x;
    std::uint8_t y;
    bool cross;
};

void check_execution() {
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        const auto op8 = static_cast<std::uint8_t>(opcode);
        const auto probe = m6502::decode(op8);
        std::vector<Scenario> scenarios{{0x04, 0x10, false}};
        if (probe.mode == Mode::abx) {
            scenarios = {{0x10, 0x00, false}, {0x90, 0x00, true}};
        } else if (probe.mode == Mode::aby || probe.mode == Mode::idy) {
            scenarios = {{0x00, 0x10, false}, {0x00, 0x90, true}};
        }

        if (probe.flow == Flow::branch) {
            // Not taken, taken within the page, taken into the next page.
            struct Case { std::uint16_t pc; std::uint8_t displacement; bool taken; int extra; };
            constexpr Case cases[] = {{0x0280, 0x10, false, 0}, {0x0280, 0x10, true, 1},
                                      {0x0280, 0x80, true, 1}, {0x02f0, 0x20, true, 2}};
            for (const Case& c : cases) {
                // Find a P that takes (or not) this branch.
                std::uint8_t p = 0;
                for (const std::uint8_t candidate : {std::uint8_t{0x00}, std::uint8_t{0xc3}}) {
                    if (m6502::branch_taken(probe.op, candidate) == c.taken) {
                        p = candidate;
                    }
                }
                Ram ram;
                ram.bytes[c.pc] = op8;
                ram.bytes[c.pc + 1u] = c.displacement;
                const auto insn = m6502::decode(op8, c.displacement);
                const Run run = execute(ram, c.pc, 0, 0, p);
                const std::string where = hex(opcode) + " (" + m6502::disassemble(insn, c.pc) + ")";
                if (run.cycles != insn.cycles + c.extra) {
                    fail(where + ": " + std::to_string(run.cycles) + " cycles in MAME, decoder " +
                         std::to_string(insn.cycles + c.extra));
                }
                const std::uint16_t expected = c.taken ? insn.branch_target(c.pc) : insn.next(c.pc);
                if (run.npc != expected) {
                    fail(where + ": went to " + hex(run.npc, 4) + ", decoder says " + hex(expected, 4));
                }
                if (run.accesses != run.cycles) {
                    fail(where + ": " + std::to_string(run.accesses) + " bus accesses in " +
                         std::to_string(run.cycles) + " cycles");
                }
            }
            continue;
        }

        for (const Scenario& s : scenarios) {
            constexpr std::uint16_t pc = 0x0280;
            Ram ram;
            std::uint8_t b1 = 0x80;
            std::uint8_t b2 = 0x12;
            if (probe.mode == Mode::idx || probe.mode == Mode::idy) {
                b1 = 0x40;
                ram.put16(0x0040, 0x1280);
                ram.put16(static_cast<std::uint8_t>(0x40 + s.x), 0x1280);
            } else if (probe.mode == Mode::ind) {
                b1 = 0x34;
                ram.put16(0x1234, 0x0600);
            }
            ram.bytes[pc] = op8;
            ram.bytes[pc + 1u] = b1;
            ram.bytes[pc + 2u] = b2;
            // A return address for RTS, and P and a return address for RTI.
            ram.bytes[0x0100u + stack + 1u] = 0x33;
            ram.bytes[0x0100u + stack + 2u] = 0x12;
            if (probe.flow == Flow::rti) {
                ram.bytes[0x0100u + stack + 1u] = 0x00;
                ram.bytes[0x0100u + stack + 2u] = 0x34;
                ram.bytes[0x0100u + stack + 3u] = 0x12;
            }
            const auto insn = m6502::decode(op8, b1, b2);
            const Run run = execute(ram, pc, s.x, s.y, 0x00);
            const std::string where = hex(opcode) + " (" + m6502::disassemble(insn, pc) +
                                      (s.cross ? ", crossing a page)" : ")");

            if (insn.flow == Flow::halt) {
                if (insn.cycles != 0 || run.cycles != 1 << 20) {
                    fail(where + ": the decoder says it halts, MAME finished it in " +
                         std::to_string(run.cycles) + " cycles");
                }
                continue;
            }

            const int expected_cycles = insn.cycles + (s.cross && insn.page_penalty ? 1 : 0);
            if (run.cycles != expected_cycles) {
                fail(where + ": " + std::to_string(run.cycles) + " cycles in MAME, decoder " +
                     std::to_string(expected_cycles));
            }
            if (run.accesses != run.cycles) {
                fail(where + ": " + std::to_string(run.accesses) + " bus accesses in " +
                     std::to_string(run.cycles) + " cycles");
            }

            std::uint16_t expected_pc = insn.next(pc);
            switch (insn.flow) {
            case Flow::jump: case Flow::call: expected_pc = insn.operand; break;
            case Flow::jump_indirect: expected_pc = ram.get16(insn.operand); break;
            case Flow::ret: expected_pc = 0x1234; break;
            case Flow::rti: expected_pc = 0x1234; break;
            case Flow::brk: expected_pc = irq_handler; break;
            default: break;
            }
            if (run.npc != expected_pc) {
                fail(where + ": went to " + hex(run.npc, 4) + ", decoder says " + hex(expected_pc, 4));
            }
            // JSR pushes the address of its last byte; BRK skips the byte after it.
            if (insn.flow == Flow::call && ram.get16(pushed_word) != pc + 2) {
                fail(where + ": pushed " + hex(ram.get16(pushed_word), 4));
            }
            if (insn.flow == Flow::brk && (ram.get16(pushed_word) != pc + 2 ||
                                           !(ram.bytes[pushed_p] & m6502::flag_b))) {
                fail(where + ": pushed " + hex(ram.get16(pushed_word), 4) + " and P " +
                     hex(ram.bytes[pushed_p]));
            }
        }
    }
}

void check_interrupts() {
    constexpr std::uint16_t pc = 0x0280;
    for (const bool nmi : {false, true}) {
        Ram ram;
        ram.bytes[pc] = 0xea;   // NOP
        ram.put16(m6502::irq_vector, irq_handler);
        ram.put16(m6502::nmi_vector, nmi_handler);
        m6502_device cpu(ram);
        cpu.set_state(pc, 0, 0, 0, 0x30, stack);
        cpu.set_input_line(nmi ? m6502_device::NMI_LINE : m6502_device::IRQ_LINE, true);
        cpu.step();                       // the NOP, which samples the line
        ram.accesses = 0;
        const int cycles = cpu.step();    // the interrupt sequence
        const char* name = nmi ? "NMI" : "IRQ";
        if (cycles != m6502::interrupt_cycles || ram.accesses != cycles) {
            fail(std::string(name) + ": " + std::to_string(cycles) + " cycles in MAME");
        }
        if (cpu.npc() != (nmi ? nmi_handler : irq_handler)) {
            fail(std::string(name) + ": went to " + hex(cpu.npc(), 4));
        }
        if (ram.get16(pushed_word) != pc + 1 || (ram.bytes[pushed_p] & m6502::flag_b)) {
            fail(std::string(name) + ": pushed " + hex(ram.get16(pushed_word), 4) + " and P " +
                 hex(ram.bytes[pushed_p]));
        }
        if (!(cpu.p() & m6502::flag_i)) {
            fail(std::string(name) + ": I is not set in the handler");
        }
    }

    Ram ram;
    ram.put16(m6502::reset_vector, reset_handler);
    m6502_device cpu(ram);
    cpu.reset();
    const int cycles = cpu.step();
    if (cycles != m6502::reset_cycles || ram.accesses != cycles || cpu.npc() != reset_handler) {
        fail("reset: " + std::to_string(cycles) + " cycles in MAME, to " + hex(cpu.npc(), 4));
    }
}

} // namespace

int main() {
    check_disassembly();
    check_execution();
    check_interrupts();

    int documented = 0;
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        documented += m6502::decode(static_cast<std::uint8_t>(opcode)).documented ? 1 : 0;
    }
    if (documented != 151) {
        fail("the decoder marks " + std::to_string(documented) + " opcodes documented, not 151");
    }

    if (failures != 0) {
        std::fprintf(stderr, "m6502 decoder check: %d disagreements with MAME\n", failures);
        return 1;
    }
    std::printf("m6502 decoder check: 256 opcodes agree with MAME's disassembler and core "
                "(%d runs, IRQ, NMI and reset)\n", runs);
    return 0;
}
