// Stands in for the code qilin lift makes from the ROMs (empty dispatch
// tables), so the player kit can be linked and started without them: CI
// builds this to show libapb_kit.a and its system libraries are complete.
#include "qilin_board.hpp"
#include "qilin_sound_board.hpp"

namespace qilin::rt {
const Entry lifted_entries[] = {{0, 0, nullptr, 0}};
const unsigned lifted_entry_count = 0;
bool dispatch(Board&) { return false; }
}  // namespace qilin::rt

namespace qilin::rt6502 {
const Entry lifted_entries[] = {{0, nullptr, 0}};
const unsigned lifted_entry_count = 0;
bool dispatch(Board&) { return false; }
}  // namespace qilin::rt6502
