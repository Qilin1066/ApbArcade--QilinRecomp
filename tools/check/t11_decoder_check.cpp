// Cross-checks the T-11 decoder against MAME, which is this project's reference.
//
// For all 65,536 opcode words:
//   - the decoder's disassembly must equal MAME's disassembler (t11dasm.cpp),
//     at several PCs and with several extension words, and its length must
//     equal MAME's for every instruction the T-11 executes;
//   - running the word once in MAME's T-11 core (t11ops.hxx) must take the
//     decoder's cycle count, and trap exactly when the decoder says it does,
//     through the vector it names.
//
// Needs no game files. Exit status 1 on any disagreement.

#include "t11_decoder.hpp"

#include "t11_reference.h"
#include "emu.h"
#include "t11dasm.h"

#include <array>
#include <cstdio>
#include <sstream>
#include <string>

namespace {

namespace t11 = qilin::t11;

struct Ram final : t11_device::Bus {
    std::array<std::uint8_t, 0x10000> bytes{};

    std::uint16_t word(std::uint16_t address) const {
        address &= 0xfffe;
        return static_cast<std::uint16_t>(bytes[address] | (bytes[address + 1u] << 8));
    }
    void put(std::uint16_t address, std::uint16_t value) {
        address &= 0xfffe;
        bytes[address] = static_cast<std::uint8_t>(value);
        bytes[address + 1u] = static_cast<std::uint8_t>(value >> 8);
    }
    std::uint16_t fetch(std::uint16_t address) override { return word(address); }
    std::uint16_t read_word(std::uint16_t address) override { return word(address); }
    void write_word(std::uint16_t address, std::uint16_t data) override { put(address, data); }
    std::uint8_t read_byte(std::uint16_t address) override { return bytes[address]; }
    void write_byte(std::uint16_t address, std::uint8_t data) override { bytes[address] = data; }
};

constexpr std::uint16_t handler(int vector) { return static_cast<std::uint16_t>(0x0400 + vector); }

int failures = 0;

void fail(const std::string& message) {
    if (++failures <= 40) {
        std::fprintf(stderr, "%s\n", message.c_str());
    }
}

std::string octal(unsigned value) {
    char text[16];
    std::snprintf(text, sizeof text, "%06o", value);
    return text;
}

void check_disassembly(std::uint16_t pc, std::uint16_t w0, std::uint16_t w1, std::uint16_t w2) {
    const std::array<std::uint16_t, 3> words{w0, w1, w2};
    const util::disasm_interface::data_buffer buffer(words.data(), pc);
    t11_disassembler mame;
    std::ostringstream text;
    const auto result = mame.disassemble(text, pc, buffer, buffer);
    const auto length = result & util::disasm_interface::LENGTHMASK;

    const auto insn = t11::decode(w0, w1, w2);
    const auto ours = t11::disassemble(insn, pc);
    if (ours != text.str()) {
        fail("text at " + octal(pc) + " for " + octal(w0) + ": MAME '" + text.str() + "', decoder '" + ours + "'");
    }
    if (insn.legal && length != 2u * insn.words) {
        fail("length of " + octal(w0) + ": MAME " + std::to_string(length) + ", decoder " + std::to_string(2 * insn.words));
    }
}

void check_execution(std::uint16_t w0) {
    constexpr std::uint16_t pc = 0x8000;
    constexpr std::uint16_t sp = 0x1000;
    constexpr std::uint8_t psw = 0;
    Ram ram;
    for (const int vector : {004, 010, 014, 020, 030, 034}) {
        ram.put(static_cast<std::uint16_t>(vector), handler(vector));
        ram.put(static_cast<std::uint16_t>(vector + 2), 0x00e0);
    }
    for (unsigned address = 0x2000; address < 0x3000; address += 2) {
        ram.put(static_cast<std::uint16_t>(address), 0x4000);
    }
    ram.put(pc, w0);
    ram.put(pc + 2, 0x0100);
    ram.put(pc + 4, 0x0200);

    t11_device cpu(ram, 0x36ff);
    cpu.reset();
    for (int r = 0; r < 6; ++r) {
        cpu.set_reg(r, static_cast<std::uint16_t>(0x2000 + 0x100 * r));
    }
    cpu.set_reg(6, sp);
    cpu.set_reg(7, pc);
    cpu.set_psw(psw);
    const int cycles = cpu.step();

    const auto insn = t11::decode(w0, 0x0100, 0x0200);
    if (cycles != insn.cycles) {
        fail("cycles of " + octal(w0) + " (" + t11::disassemble(insn, pc) + "): MAME " + std::to_string(cycles) +
             ", decoder " + std::to_string(insn.cycles));
    }

    const int vector = t11::trap_vector(insn);
    const bool trapped = cpu.reg(6) == sp - 4 && ram.word(sp - 4) == pc + 2 && ram.word(sp - 2) == psw &&
                         cpu.reg(7) == handler(vector < 0 ? 0 : vector);
    if (vector >= 0 && !trapped) {
        fail("expected " + octal(w0) + " (" + t11::disassemble(insn, pc) + ") to trap through " + octal(static_cast<unsigned>(vector)));
    }
    if (vector < 0 && (cpu.reg(7) & 0xff00) == 0x0400) {
        fail("unexpected trap for " + octal(w0) + " (" + t11::disassemble(insn, pc) + "), PC " + octal(cpu.reg(7)));
    }
}

} // namespace

int main() {
    static constexpr std::array<std::array<std::uint16_t, 2>, 4> extensions{{
        {0x0100, 0x0200}, {0xfff0, 0x8000}, {0x7fff, 0x0001}, {0x00fe, 0xffff},
    }};
    static constexpr std::array<std::uint16_t, 3> pcs{0x8000, 0xfffa, 0x0000};
    unsigned legal = 0;
    for (unsigned w0 = 0; w0 < 0x10000; ++w0) {
        for (const auto pc : pcs) {
            for (const auto& ext : extensions) {
                check_disassembly(pc, static_cast<std::uint16_t>(w0), ext[0], ext[1]);
            }
        }
        check_execution(static_cast<std::uint16_t>(w0));
        legal += t11::decode(static_cast<std::uint16_t>(w0)).legal;
    }
    if (failures != 0) {
        std::fprintf(stderr, "T-11 decoder check: %d disagreements with MAME\n", failures);
        return 1;
    }
    std::printf("T-11 decoder agrees with MAME on all 65536 opcode words (%u execute on the T-11): "
                "disassembly, length, cycles and traps.\n", legal);
    return 0;
}
