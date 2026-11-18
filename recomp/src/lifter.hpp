#pragma once
// T-11 -> C++ lifter.
//
// Each function the analysis found becomes  void fn_XXXX(rt::Board&, unsigned entry)
// (recomp/runtime/t11_rt.hpp). Its body is every location reachable from its
// entry without crossing a call, as labels and gotos. R0-R6 and the flags are
// locals; PC is a constant. Every instruction makes its bus calls in the
// order MAME's T-11 core makes them, fetches included, charges MAME's cycle
// count, and computes its flags with MAME's own expressions.
//
// Calls are native C++ calls; the caller checks where its callee returned to,
// and if that isn't the instruction after the call (a coroutine, a frame the
// callee rewrote, a bank the callee switched), it returns too, leaving the
// state in Board::cpu, until some caller expects that address or the
// top-level dispatcher continues there.

#include "cfg.hpp"

#include <string>
#include <vector>

namespace qilin::lift {

struct File {
    std::string name;
    std::string text;
};

struct Result {
    std::vector<File> files;           // lifted_N.cpp, lifted_dispatch.cpp, lifted.hpp
    std::vector<std::string> failures; // instructions that can't be lifted; empty on success
    std::size_t functions = 0;
    std::size_t instructions = 0;      // lifted, counting code shared by several functions once per function
};

struct Options {
    unsigned service_interval = 128;   // most cycles a straight run goes without a service check
    std::size_t functions_per_file = 64;
};

[[nodiscard]] Result lift(const cfg::Program& program, const Options& options = {});

// The C++ name of the function starting at 'entry'.
[[nodiscard]] std::string function_name(cfg::Loc entry);

} // namespace qilin::lift
