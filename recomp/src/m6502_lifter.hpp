#pragma once
// 6502 -> C++ lifter, for the sound program.
//
// Each function the analysis found becomes  void snd_XXXX(rt6502::Board&, unsigned entry)
// (recomp/runtime/m6502_rt.hpp): every location reachable from its entry
// without crossing a call, as labels and gotos. Each instruction's body is
// MAME's microcode for its opcode (om6502.lst, vendored in
// third_party/mame-m6502 and built into qilin), translated token by token:
// MAME's registers become locals and its bus calls become board calls that
// count one cycle each. The bus therefore sees what MAME's core does, in
// MAME's order, dummy reads and page-crossing reads included.
//
// The PC after an instruction is a local the compiler folds to a constant;
// the lifted code then goes to the label of the address it holds, among the
// successors the analysis found, or leaves through dispatch if it is none of
// them (a branch the analysis proved one-way that went the other way).
// Calls are native C++ calls whose callers check where they came back to, as
// for the T-11; an RTS through a table goes straight to its targets' labels.

#include "m6502_cfg.hpp"

#include <optional>
#include <string>
#include <vector>

namespace qilin::lift6502 {

struct File {
    std::string name;
    std::string text;
};

struct Result {
    std::vector<File> files;           // sound_N.cpp, sound_dispatch.cpp, sound_lifted.hpp
    std::vector<std::string> failures; // instructions that can't be lifted; empty on success
    std::size_t functions = 0;
    std::size_t instructions = 0;      // lifted, counting code shared by several functions once per function
};

struct Options {
    unsigned service_interval = 64;    // most cycles a straight run goes without a service check
    std::size_t functions_per_file = 32;
};

[[nodiscard]] Result lift(const m6502::Program& program, const Options& options = {});

// The C++ name of the function starting at 'entry'.
[[nodiscard]] std::string function_name(std::uint16_t entry);

// MAME's microcode for an opcode as lifted C++ statements, without its final
// opcode fetch's PC increment (lifted code knows where it goes). Empty if it
// uses machine state lifted code doesn't model (BRK's interrupt flags, KIL).
[[nodiscard]] std::optional<std::string> microcode(std::uint8_t opcode);

} // namespace qilin::lift6502
