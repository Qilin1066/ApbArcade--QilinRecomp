#pragma once
// qilin coverage: every ROM word of both CPUs accounted for.
//
// Each word is one of:
//   code     an opcode or operand word of an instruction the analysis reached
//   table    an entry of a jump table it sized
//   data     from a ROM address code refers to (an immediate, an absolute or
//            relative address, an index base) up to the next code or table
//   fill     runs of four or more 0x0000 or 0xFFFF words (T-11), or 0x00 or
//            0xFF bytes (6502)
//   data bank  a T-11 bank no code runs in (its contents are read, not run)
//   unknown  none of these
// and an unknown stretch that decodes like real code from its start is
// listed as a candidate: code the analysis may have missed. Those, and a
// reference run's seeds that land outside known code, are what the analysis
// has yet to explain.

#include "cfg.hpp"
#include "romset.hpp"

#include <string>
#include <vector>

namespace qilin::coverage {

struct Options {
    bool list = false;                 // every unknown stretch, not just the totals
    std::vector<cfg::Loc> seeds;       // the seeds the analysis starts from
    std::string check_seeds;           // a seed file to place against the analysis, or empty
};

// Prints the report; returns 3 if any unknown stretch looks like code, else 0.
int report(const rom::RomSet& set, const Options& options);

} // namespace qilin::coverage
