// Builds the lifter check's program: a synthetic fixed ROM with one small
// function per T-11 instruction form, then analyses and lifts it with the
// real recompiler.
//
//   qilin_lifter_fixture <out-dir>
//
// writes program.bin (MAME's 0x90000-byte maincpu region, of which only the
// fixed ROM is used) and the lifted C++ (lifted.hpp, lifted_*.cpp,
// lifted_dispatch.cpp). tools/check/lifter_check.cpp runs every function
// against MAME's core. No game data is involved.

#include "cfg.hpp"
#include "lifter.hpp"
#include "romset.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <initializer_list>
#include <vector>

namespace {

using namespace qilin;

// Stubs at fixed addresses: RTS Rn for each link register, and RTI.
constexpr std::uint16_t rts_stub = 0x8000;           // + 2n: RTS Rn
constexpr std::uint16_t rti_stub = 0x8010;
constexpr std::uint16_t pointer_to_rts = 0x8020;     // a word holding rts_stub + 14 (RTS PC)
constexpr std::uint16_t data_word = 0x8022;          // a ROM word for reads
constexpr std::uint16_t code_start = 0x8100;

struct Builder {
    std::vector<std::uint8_t> region = std::vector<std::uint8_t>(0x90000, 0);
    std::vector<cfg::Loc> functions;
    std::uint16_t pc = code_start;

    void put(std::uint16_t address, std::uint16_t word) {
        region[address] = static_cast<std::uint8_t>(word);
        region[address + 1u] = static_cast<std::uint8_t>(word >> 8);
    }
    void emit(std::uint16_t word) {
        put(pc, word);
        pc = static_cast<std::uint16_t>(pc + 2);
    }
    void begin() { functions.push_back(cfg::Loc{pc, 0}); }
    void end() { emit(000207); }   // RTS PC
};

// Extension words for an operand, given where it will sit.
// R7 forms: #imm, @#a, a (relative), @a (relative deferred), (PC).
struct OperandForm {
    unsigned field;   // mode << 3 | reg
    std::uint16_t word;
    bool relative;    // word is an address to make PC-relative
};

std::vector<OperandForm> operand_forms(bool destination, unsigned rotate) {
    std::vector<OperandForm> forms;
    for (unsigned mode = 0; mode < 8; ++mode) {
        const unsigned reg = (mode * 3 + rotate) % 7;   // R0-R6, varied
        const std::uint16_t index = std::initializer_list<std::uint16_t>{0x0010, 0xFFFE, 0x0001, 0x8000}.begin()[(mode + rotate) % 4];
        forms.push_back({mode << 3 | reg, index, false});
    }
    // R7 forms. A destination (PC)+ is read as data: include it.
    forms.push_back({027, std::initializer_list<std::uint16_t>{0x0000, 0x0001, 0x007F, 0x0080, 0x00FF, 0x7FFF, 0x8000, 0xFFFF}.begin()[rotate % 8], false});
    forms.push_back({037, static_cast<std::uint16_t>(rotate % 2 ? 0x0201 : 0x0200), false});   // @#a in RAM, even and odd
    forms.push_back({067, 0x0300, true});                // a, relative, into RAM
    forms.push_back({077, data_word, true});             // @a: the pointer comes from ROM
    if (!destination) forms.push_back({017, 0, false});  // (PC): the next word, read as data
    return forms;
}

void emit_operand_words(Builder& b, const OperandForm& form, bool has_word) {
    if (!has_word) return;
    if (form.relative) {
        b.emit(static_cast<std::uint16_t>(form.word - (b.pc + 2)));
    } else {
        b.emit(form.word);
    }
}

bool has_word(unsigned field) {
    const unsigned mode = field >> 3, reg = field & 7;
    return mode >= 6 || (reg == 7 && (mode == 2 || mode == 3));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: qilin_lifter_fixture <out-dir>\n");
        return 2;
    }
    Builder b;
    for (unsigned n = 0; n < 8; ++n) b.put(static_cast<std::uint16_t>(rts_stub + 2 * n), static_cast<std::uint16_t>(000200 | n));
    b.put(rti_stub, 000002);
    b.put(pointer_to_rts, rts_stub + 14);
    b.put(data_word, 0x0400);   // @a reads this as a pointer into RAM

    // Two-operand instructions: every source mode against every destination
    // mode, and the R7 forms on each side.
    const std::uint16_t two_ops[] = {010000, 020000, 030000, 040000, 050000, 060000,
                                     0110000, 0120000, 0130000, 0140000, 0150000, 0160000};
    unsigned rotate = 0;
    for (const auto op : two_ops) {
        const auto sources = operand_forms(false, rotate);
        for (const auto& s : sources) {
            const auto destinations = operand_forms(true, ++rotate);
            for (const auto& d : destinations) {
                // Writing PC through a two-operand instruction is a computed jump: tested elsewhere.
                if ((d.field & 7) == 7 && (d.field >> 3) == 0) continue;
                b.begin();
                b.emit(static_cast<std::uint16_t>(op | s.field << 6 | d.field));
                emit_operand_words(b, s, has_word(s.field));
                emit_operand_words(b, d, has_word(d.field));
                b.end();
            }
        }
    }
    // One-operand instructions, word and byte, every mode.
    const std::uint16_t one_ops[] = {005000, 005100, 005200, 005300, 005400, 005500, 005600, 005700,
                                     006000, 006100, 006200, 006300, 000300, 006700,
                                     0105000, 0105100, 0105200, 0105300, 0105400, 0105500, 0105600, 0105700,
                                     0106000, 0106100, 0106200, 0106300, 0106400, 0106700};
    for (const auto op : one_ops) {
        for (const auto& d : operand_forms(true, ++rotate)) {
            if ((d.field & 7) == 7 && (d.field >> 3) == 0) continue;
            b.begin();
            b.emit(static_cast<std::uint16_t>(op | d.field));
            emit_operand_words(b, d, has_word(d.field));
            b.end();
        }
    }
    // XOR, every register and mode.
    for (const auto& d : operand_forms(true, ++rotate)) {
        if ((d.field & 7) == 7 && (d.field >> 3) == 0) continue;
        b.begin();
        b.emit(static_cast<std::uint16_t>(074000 | ((rotate % 7) << 6) | d.field));
        emit_operand_words(b, d, has_word(d.field));
        b.end();
    }
    // Condition codes, MFPT, NOP.
    for (unsigned mask = 0; mask < 16; ++mask) {
        for (const std::uint16_t op : {std::uint16_t{000240}, std::uint16_t{000260}}) {
            b.begin();
            b.emit(static_cast<std::uint16_t>(op | mask));
            b.end();
        }
    }
    b.begin();
    b.emit(000007);   // MFPT
    b.end();
    b.begin();
    b.emit(000005);   // RESET
    b.end();
    // Branches, forward over one instruction and backward.
    for (std::uint16_t op = 000400; op < 004000; op += 000400) {
        for (const std::uint16_t base : {std::uint16_t{0}, std::uint16_t{0100000}}) {
            const auto code = static_cast<std::uint16_t>(base | op);
            b.begin();
            b.emit(static_cast<std::uint16_t>(code | 1));   // Bxx .+4
            b.emit(005200);                                 // INC R0 (skipped when taken)
            b.end();
            // Backward: BR fwd; back: RTS PC; fwd: Bxx back; INC R1; RTS PC
            b.begin();
            b.emit(000401);
            b.emit(000207);
            b.emit(static_cast<std::uint16_t>(code | 0376));   // back to the RTS PC
            b.emit(005201);
            b.end();
        }
    }
    for (std::uint16_t op = 0100000; op < 0104000; op += 000400) {
        b.begin();
        b.emit(static_cast<std::uint16_t>(op | 1));
        b.emit(005200);
        b.end();
    }
    // SOB on each register: L: INC R5 / SOB Rn,L
    for (unsigned r = 0; r < 5; ++r) {
        b.begin();
        b.emit(005205);
        b.emit(static_cast<std::uint16_t>(077000 | r << 6 | 2));
        b.end();
    }
    // JMP and JSR with fixed targets: @#a, a (relative), @a through ROM.
    for (const bool call : {false, true}) {
        for (unsigned link = 0; link < 8; ++link) {
            if (!call && link > 0) break;
            const std::uint16_t base = call ? static_cast<std::uint16_t>(004000 | link << 6) : std::uint16_t{000100};
            const auto target = static_cast<std::uint16_t>(rts_stub + 2 * (call ? link : 7));
            b.begin();
            b.emit(static_cast<std::uint16_t>(base | 037));
            b.emit(target);
            b.end();
            b.begin();
            b.emit(static_cast<std::uint16_t>(base | 067));
            b.emit(static_cast<std::uint16_t>(target - (b.pc + 2)));
            b.end();
            if (call && link == 7) {
                b.begin();
                b.emit(static_cast<std::uint16_t>(base | 077));
                b.emit(static_cast<std::uint16_t>(pointer_to_rts - (b.pc + 2)));
                b.end();
            }
        }
    }
    // The coroutine call APB's save helpers use: JSR PC,@(SP)+ back into us.
    b.begin();
    b.emit(012746);                                          // MOV #L,-(SP)
    b.emit(static_cast<std::uint16_t>(b.pc + 4));
    b.emit(004736);                                          // JSR PC,@(SP)+ -> L, pushing the return
    b.emit(005725);                                          // L: TST (R5)+ ; then RTS PC returns to it
    b.end();
    // Traps: through vectors the check points at the RTI stub.
    for (const std::uint16_t op : {std::uint16_t{000003}, std::uint16_t{000004}, std::uint16_t{0104005},
                                   std::uint16_t{0104405}, std::uint16_t{0070102}, std::uint16_t{000107}}) {
        b.begin();
        b.emit(op);
        b.end();
    }
    if (b.pc >= 0xF000) {
        std::fprintf(stderr, "qilin_lifter_fixture: the program doesn't fit\n");
        return 1;
    }

    std::array<std::vector<std::uint8_t>, rom::region_count> data;
    std::array<std::vector<bool>, rom::region_count> populated;
    for (std::size_t r = 0; r < rom::region_count; ++r) {
        data[r].assign(rom::region_info(static_cast<rom::Region>(r)).size, 0);
        populated[r].assign(data[r].size(), false);
    }
    data[0] = b.region;
    for (std::uint32_t i = 0x8000; i < 0x10000; ++i) populated[0][i] = true;
    const auto set = rom::RomSet::from_regions(data, populated);
    // The stubs are entries too: traps and computed calls dispatch to them.
    auto seeds = b.functions;
    for (unsigned n = 0; n < 8; ++n) seeds.push_back(cfg::Loc{static_cast<std::uint16_t>(rts_stub + 2 * n), 0});
    seeds.push_back(cfg::Loc{rti_stub, 0});
    const auto program = cfg::Program::analyze(set, seeds);
    // Exactly eight source files, so the build knows their names in advance.
    lift::Options options;
    options.functions_per_file = (seeds.size() + 7) / 8;
    const auto result = lift::lift(program, options);
    if (!result.failures.empty()) {
        for (const auto& f : result.failures) std::fprintf(stderr, "qilin_lifter_fixture: can't lift %s\n", f.c_str());
        return 1;
    }
    const std::filesystem::path out(argv[1]);
    std::filesystem::create_directories(out);
    for (unsigned k = 0; k < 8; ++k) {   // files the lifter didn't need stay valid, empty units
        std::ofstream(out / ("lifted_" + std::to_string(k) + ".cpp"), std::ios::binary | std::ios::trunc)
            << "// (no functions)\n";
    }
    for (const auto& file : result.files) {
        std::ofstream(out / file.name, std::ios::binary | std::ios::trunc) << file.text;
    }
    std::ofstream(out / "program.bin", std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(b.region.data()), static_cast<std::streamsize>(b.region.size()));
    std::printf("lifter fixture: %zu functions, %zu instructions lifted\n", b.functions.size(), result.instructions);
    return 0;
}
