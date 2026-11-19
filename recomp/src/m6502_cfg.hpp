#pragma once
// Control flow of the 6502 sound program: recursive descent over its ROM,
// driven by an abstract interpretation of the registers, flags and stack.
//
// The sound CPU's address space is flat (RAM 0x0000-0x0FFF, devices
// 0x1000-0x1FFF, all of that mirrored at 0x2000-0x3FFF, ROM 0x4000-0xFFFF),
// so a code location is just its address. Exploration starts at the reset,
// NMI and IRQ vectors.
//
// A, X and Y are tracked as strided intervals of byte values (with the ROM
// table and index each came from, when loaded through one), N, Z, C and V as
// known or unknown, and the bytes each function has pushed as a list. Which
// register or comparison last set the flags is remembered too, so a
// conditional branch narrows the register on each edge. That gives:
//   - branches whose condition is known: only the feasible edge is followed
//     (the code after "LDA #$FF / BNE" is often data);
//   - the "RTS trick", a jump through two bytes pushed from ROM tables
//     indexed by a register of known range: every entry is listed.
// Discovery and the interpretation run as one fixpoint: an edge is followed
// once a state reaching it allows it, and states only grow, so the result
// covers every execution from the vectors.
//
// Everything the analysis can't follow is listed, never guessed.

#include "m6502_decoder.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace qilin::m6502 {

// How an instruction passes control on, as far as the analysis knows.
enum class Transfer : std::uint8_t {
    next,           // to the following instruction only
    branch,         // conditional: see Node::next for the edges that are feasible
    jump,           // JMP to a known target
    call,           // JSR; the callee may return to next
    computed_jump,  // JMP (ind), or an RTS the analysis can't resolve
    table,          // RTS through an address pushed from ROM tables: entries listed
    ret,            // RTS
    rti,
    brk,            // through the IRQ vector, with B set
    halt,           // KIL
};

struct Node {
    Instruction insn;
    Transfer transfer = Transfer::next;
    std::uint16_t target = 0;               // branch, jump or call target
    std::vector<std::uint16_t> next;        // feasible successors, call targets excepted
    std::vector<std::uint16_t> table;       // a jump table: its entries' targets
    // A branch one of whose edges no execution takes (its condition is known
    // there). The lifted code still tests it, and stops if it ever goes that way.
    bool never_taken = false;
    bool always_taken = false;
    // A table RTS that also returns to its caller on some paths.
    bool returns = false;
};

enum class Issue : std::uint8_t {
    computed_jump,      // JMP (ind), or RTS through pushed bytes of unknown origin
    unknown_stack,      // RTS where the bytes pushed since the function's entry aren't known
    pops_return,        // pulls more than it pushed: reads or drops its caller's return address
    implausible_entry,  // a slot of an RTS table within the index range that doesn't decode as code
    undocumented,       // an undocumented opcode
    brk,
    halt,               // KIL
    outside_rom,        // control leaves ROM (code in RAM or a device)
    overlap,            // an address decoded both as an opcode and as an operand byte
};

struct Finding {
    Issue issue;
    std::uint16_t at;
    std::string detail;
};

class Program {
public:
    static constexpr std::uint16_t rom_start = 0x4000;

    // memory: the CPU's 64 KiB view (only the ROM part is read).
    static Program analyze(std::span<const std::uint8_t> memory, const std::vector<std::uint16_t>& seeds = {});

    [[nodiscard]] const std::map<std::uint16_t, Node>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::set<std::uint16_t>& functions() const noexcept { return functions_; }
    [[nodiscard]] const std::set<std::uint16_t>& return_sites() const noexcept { return return_sites_; }
    [[nodiscard]] const std::vector<Finding>& findings() const noexcept { return findings_; }
    // Starts of code no other entry reaches, lifted as entries anyway.
    [[nodiscard]] const std::set<std::uint16_t>& gap_entries() const noexcept { return gap_entries_; }
    [[nodiscard]] std::uint16_t reset_entry() const noexcept { return vector(reset_vector); }
    [[nodiscard]] std::uint16_t nmi_entry() const noexcept { return vector(nmi_vector); }
    [[nodiscard]] std::uint16_t irq_entry() const noexcept { return vector(irq_vector); }

private:
    friend struct Interpreter;
    static Program analyze_once(std::span<const std::uint8_t> memory, const std::vector<std::uint16_t>& seeds);
    [[nodiscard]] std::uint8_t byte(std::uint32_t address) const noexcept {
        return address < memory_.size() ? memory_[address] : 0;
    }
    [[nodiscard]] std::uint16_t vector(std::uint16_t address) const noexcept {
        return static_cast<std::uint16_t>(byte(address) | (byte(address + 1u) << 8));
    }

    std::span<const std::uint8_t> memory_;
    std::map<std::uint16_t, Node> nodes_;
    std::set<std::uint16_t> gap_entries_;
    std::set<std::uint16_t> functions_;
    std::set<std::uint16_t> return_sites_;
    std::vector<Finding> findings_;
};

[[nodiscard]] const char* issue_name(Issue issue) noexcept;

} // namespace qilin::m6502
