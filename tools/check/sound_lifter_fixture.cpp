// Builds the 6502 lifter check's program: a synthetic sound ROM with one
// small function per 6502 instruction form, then analyses and lifts it with
// the real recompiler.
//
//   qilin_sound_lifter_fixture <out-dir>
//
// writes program.bin (the 64 KiB address space; ROM from 0x4000) and the
// lifted C++ (sound_lifted.hpp, sound_0..7.cpp, sound_dispatch.cpp).
// tools/check/sound_lifter_check.cpp runs every function against MAME's
// 6502 core. No game data is involved.
//
// The forms: every documented opcode in every addressing mode it has, with
// operands in RAM and (for reads) in ROM, indexed so that some trials cross
// a page; branches both ways, forward and back, and across a page; a DEX/BNE
// loop (service points and yields); JMP absolute and indirect (with the NMOS
// page-wrap of a pointer at xxFF); JSR/RTS; the stack instructions; an RTS
// through constant pushes and through a four-entry table indexed from RAM;
// and NMI and IRQ handlers ending in RTI.

#include "m6502_cfg.hpp"
#include "m6502_decoder.hpp"
#include "m6502_lifter.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using namespace qilin;
namespace m6502 = qilin::m6502;

constexpr std::uint16_t data_area = 0x7000;   // ROM bytes for reads; 0x70FF/0x7000 for JMP's page wrap
constexpr std::uint16_t code_start = 0x4000;

struct Builder {
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(0x10000, 0xEA);   // NOP filler
    std::vector<std::uint16_t> functions;
    std::uint16_t pc = code_start;

    void emit(std::initializer_list<int> bytes) {
        for (const int b : bytes) memory[pc++] = static_cast<std::uint8_t>(b);
    }
    void begin() { functions.push_back(pc); }
    void rts() { emit({0x60}); }
    // Leaves room so the next function starts 'before' bytes short of a page boundary.
    void align_before_page(unsigned before) {
        unsigned next = (pc | 0xffu) + 1u;
        if (next - before < pc) next += 0x100;
        pc = static_cast<std::uint16_t>(next - before);
    }
};

// Operand bytes for an addressing mode, in a few variants.
std::vector<std::vector<int>> operands(m6502::Mode mode, m6502::Access access) {
    const bool reads = access == m6502::Access::read;
    switch (mode) {
    case m6502::Mode::imm: return {{0x00}, {0x01}, {0x7F}, {0x80}, {0xFF}};
    case m6502::Mode::zpg: return {{0x10}};
    case m6502::Mode::zpx: case m6502::Mode::zpy: return {{0xF0}};   // wraps in page zero for X >= 0x10
    case m6502::Mode::idx: return {{0x20}};
    case m6502::Mode::idy: return {{0x30}};
    case m6502::Mode::aba:
        if (reads) return {{0x34, 0x02}, {0x10, data_area >> 8}};
        return {{0x34, 0x02}};
    case m6502::Mode::abx: case m6502::Mode::aby:
        if (reads) return {{0xF0, 0x02}, {0xF0, data_area >> 8}};   // crossing a page for index >= 0x10
        return {{0xF0, 0x02}};
    default: return {{}};
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: qilin_sound_lifter_fixture <out-dir>\n");
        return 2;
    }
    Builder b;

    // Every documented opcode with a plain operand form.
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        const auto insn = m6502::decode(static_cast<std::uint8_t>(opcode));
        if (!insn.documented || insn.flow != m6502::Flow::next) continue;
        // Below, in balanced pairs and with a valid stack pointer.
        if (insn.op == m6502::Op::txs || insn.op == m6502::Op::pha || insn.op == m6502::Op::php ||
            insn.op == m6502::Op::pla || insn.op == m6502::Op::plp) {
            continue;
        }
        for (const auto& operand : operands(insn.mode, insn.access)) {
            b.begin();
            b.memory[b.pc++] = static_cast<std::uint8_t>(opcode);
            for (const int byte : operand) b.memory[b.pc++] = static_cast<std::uint8_t>(byte);
            b.rts();
        }
    }
    // Stack instructions in balanced pairs; TXS with the stack pointer TSX read.
    for (const auto& pair : {std::initializer_list<int>{0x48, 0x68}, {0x08, 0x28}, {0x08, 0x68}, {0x48, 0x28},
                             {0xBA, 0x9A}}) {
        b.begin();
        b.emit(pair);
        b.rts();
    }
    // Branches: forward over an RTS, and back to one.
    const std::uint8_t branches[] = {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0};
    for (const auto op : branches) {
        b.begin();
        b.emit({op, 0x01, 0x60, 0x60});                          // Bxx +1; RTS; RTS
        b.begin();
        const auto ahead = static_cast<std::uint16_t>(b.pc + 4);
        b.emit({0x4C, ahead & 0xff, ahead >> 8, 0x60, op, 0xFD, 0x60});   // JMP ahead; back: RTS; ahead: Bxx back; RTS
    }
    // Across a page: the branch's next instruction is the page's last byte.
    for (const auto op : branches) {
        b.align_before_page(3);
        b.begin();
        b.emit({op, 0x02, 0x60, 0xEA, 0x60});                    // target in the next page
    }
    // A counted loop: back edges, service points, yields.
    b.begin();
    b.emit({0xCA, 0xD0, 0xFD, 0x60});                            // DEX; BNE -3; RTS
    b.begin();
    b.emit({0x88, 0xEA, 0xEA, 0xD0, 0xFB, 0x60});                // DEY; NOP; NOP; BNE; RTS
    // JMP absolute, and indirect through ROM pointers (one at xxFF: NMOS page wrap).
    b.begin();
    {
        const auto target = static_cast<std::uint16_t>(b.pc + 3);
        b.emit({0x4C, target & 0xff, target >> 8, 0x60});
    }
    b.begin();
    const auto indirect_target = static_cast<std::uint16_t>(b.pc + 3);
    b.emit({0x6C, 0x00, (data_area >> 8) + 1, 0x60});            // JMP ($7100) -> next RTS
    b.memory[data_area + 0x100] = static_cast<std::uint8_t>(indirect_target);
    b.memory[data_area + 0x101] = static_cast<std::uint8_t>(indirect_target >> 8);
    b.begin();
    const auto wrap_target = static_cast<std::uint16_t>(b.pc + 3);
    b.emit({0x6C, 0xFF, (data_area >> 8) + 2, 0x60});            // JMP ($72FF): high byte from $7200
    b.memory[data_area + 0x2FF] = static_cast<std::uint8_t>(wrap_target);
    b.memory[data_area + 0x200] = static_cast<std::uint8_t>(wrap_target >> 8);
    b.memory[data_area + 0x300] = 0xEE;                          // where a carrying 6502 would read
    // JSR to a function that pushes and pulls.
    const std::uint16_t callee = 0x6F00;
    b.begin();
    b.emit({0x20, callee & 0xff, callee >> 8, 0xE8, 0x60});      // JSR; INX; RTS
    b.memory[callee] = 0x48;                                     // PHA; PLA; RTS
    b.memory[callee + 1u] = 0x68;
    b.memory[callee + 2u] = 0x60;
    // RTS through constant pushes.
    b.begin();
    {
        const auto target = static_cast<std::uint16_t>(b.pc + 6);
        const auto back = static_cast<std::uint16_t>(target - 1);
        b.emit({0xA9, back >> 8, 0x48, 0xA9, back & 0xff, 0x48, 0x60, 0x60});
    }
    // RTS through a four-entry table indexed by a byte from RAM.
    b.begin();
    {
        const std::uint16_t start = b.pc;
        constexpr std::uint16_t lo = data_area + 0x400, hi = data_area + 0x404;
        b.emit({0xA6, 0x40, 0xE0, 0x04, 0xB0, 0x0A,              // LDX $40; CPX #4; BCS out
                0xBD, hi & 0xff, hi >> 8, 0x48,                  // LDA hi,X; PHA
                0xBD, lo & 0xff, lo >> 8, 0x48, 0x60,            // LDA lo,X; PHA; RTS
                0x60,                                            // out: RTS
                0xE8, 0x60, 0xC8, 0x60, 0xCA, 0x60, 0x88, 0x60});   // the four targets
        for (unsigned k = 0; k < 4; ++k) {
            const auto target = static_cast<std::uint16_t>(start + 16 + 2 * k - 1);
            b.memory[lo + k] = static_cast<std::uint8_t>(target);
            b.memory[hi + k] = static_cast<std::uint8_t>(target >> 8);
        }
    }
    // Interrupt handlers, and a reset entry.
    const std::uint16_t nmi = b.pc;
    b.begin();
    b.emit({0x48, 0x8A, 0x48, 0xA9, 0x55, 0x68, 0xAA, 0x68, 0x40});   // PHA; TXA; PHA; LDA #$55; PLA; TAX; PLA; RTI
    const std::uint16_t irq = b.pc;
    b.begin();
    b.emit({0xD8, 0xE6, 0x10, 0x40});                            // CLD; INC $10; RTI
    const std::uint16_t reset = b.pc;
    b.begin();
    b.rts();
    if (b.pc >= callee) {
        std::fprintf(stderr, "qilin_sound_lifter_fixture: the code ran into the callee and data area\n");
        return 1;
    }
    for (const auto& [at, target] : {std::pair{0xFFFAu, nmi}, {0xFFFCu, reset}, {0xFFFEu, irq}}) {
        b.memory[at] = static_cast<std::uint8_t>(target);
        b.memory[at + 1] = static_cast<std::uint8_t>(target >> 8);
    }

    const auto program = m6502::Program::analyze(b.memory, b.functions);
    // TXS forgets what was pushed, so the RTS after TSX/TXS is reported; anything else is a fixture or analysis bug.
    bool unexpected = false;
    for (const auto& f : program.findings()) {
        if (f.issue == m6502::Issue::unknown_stack) continue;
        unexpected = true;
        std::fprintf(stderr, "qilin_sound_lifter_fixture: %04X %s: %s\n", f.at, m6502::issue_name(f.issue), f.detail.c_str());
    }
    lift6502::Options options;
    options.functions_per_file = (program.functions().size() + 7) / 8;
    const auto result = lift6502::lift(program, options);
    const std::filesystem::path out(argv[1]);
    std::filesystem::create_directories(out);
    for (unsigned k = 0; k < 8; ++k) {   // the build expects exactly sound_0..7.cpp
        std::ofstream(out / ("sound_" + std::to_string(k) + ".cpp"), std::ios::trunc) << "// (no functions)\n";
    }
    for (const auto& file : result.files) {
        std::ofstream(out / file.name, std::ios::binary | std::ios::trunc) << file.text;
    }
    std::ofstream(out / "program.bin", std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(b.memory.data()), static_cast<std::streamsize>(b.memory.size()));
    if (!result.failures.empty() || unexpected) {
        for (const auto& f : result.failures) std::fprintf(stderr, "qilin_sound_lifter_fixture: can't lift %s\n", f.c_str());
        return 1;   // program.bin is written, to look at
    }
    std::printf("qilin_sound_lifter_fixture: %zu functions, %zu instructions lifted into %s\n", result.functions,
                result.instructions, out.string().c_str());
    return 0;
}
