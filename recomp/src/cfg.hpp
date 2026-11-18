#pragma once
// Control flow of the T-11 program: recursive descent over the ROM.
//
// A code location is a CPU address plus the bank in that address's window
// (0 for the fixed ROM), which together name one physical ROM word. The bank
// registers are not part of a location: where they matter (control entering
// the other window, or code switching its own window) the analysis uses
// their value if it is a known constant at that point, and otherwise records
// a dynamic transfer the runtime resolves by dispatching on the bank.
//
// Everything the analysis can't follow is listed, never guessed: computed
// jumps, transfers into a window whose bank isn't known, instructions that
// trap, and PC-relative forms the T-11 allows but compilers never emit.

#include "romset.hpp"
#include "t11_decoder.hpp"

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace qilin::cfg {

struct Loc {
    std::uint16_t pc = 0;
    std::uint8_t bank = 0;   // bank of pc's window; 0 in the fixed ROM

    auto operator<=>(const Loc&) const = default;

    // Window 0 (0x4000-0x5FFF), 1 (0x6000-0x7FFF), or -1 for the fixed ROM.
    [[nodiscard]] int window() const noexcept { return pc >= 0x8000 ? -1 : (pc >= 0x6000 ? 1 : 0); }
    [[nodiscard]] std::uint32_t physical() const noexcept;
    [[nodiscard]] std::string name() const;      // "8000", "4A20/b12"
};

// What is known about a bank register at a point: a bank number, or not known.
struct Banks {
    static constexpr int unknown = -1;
    int b[2] = {unknown, unknown};
    auto operator<=>(const Banks&) const = default;
};

// How an instruction passes control on.
enum class Flow : std::uint8_t {
    next,          // to the following instruction only
    branch,        // conditional: target or next
    jump,          // to a known target only
    call,          // JSR to a known target; the callee may return to next
    call_dynamic,  // JSR to a target computed at run time
    jump_dynamic,  // JMP, or a write to PC, computed at run time
    table,         // a computed jump through a ROM table whose size is proven
    ret,           // RTS
    rti,           // RTI, RTT
    trap,          // EMT, TRAP, BPT, IOT, or a reserved instruction: through a vector
    halt,          // HALT (the T-11 restarts at its start address + 4)
    wait,          // WAIT: resumes after an interrupt
};

struct Node {
    t11::Instruction insn;
    Flow flow = Flow::next;
    std::optional<Loc> target;          // branch, jump or call target, when known
    std::vector<Loc> next;              // where control continues, call targets excepted
    std::vector<std::uint16_t> table;   // a jump table: its entries' targets
    bool heuristic_table = false;       // sized by reading entries, not by proof: checked at run time
    std::optional<int> window_entry;    // a target in this window whose bank isn't known here
    Banks banks_in;                     // bank registers on entry
    bool writes_bank = false;           // writes a ROM bank selector
};

enum class Issue : std::uint8_t {
    computed_jump,     // JMP or a write to PC through a register or memory
    computed_call,     // JSR through a register or memory
    unknown_bank,      // control enters a window whose bank isn't known
    odd_pc_mode,       // (PC), -(PC), @-(PC): legal, never compiler output
    trap,              // a trap instruction
    halt,
    outside_rom,       // control leaves ROM (RAM code, or 0x0000-0x3FFF)
    missing_rom,       // no ROM at a code address
    overlap,           // a location decoded as an operand word of another instruction
    byte_bank_write,   // a byte write to a bank selector
};

struct Finding {
    Issue issue;
    Loc at;
    std::string detail;
};

class Program {
public:
    // Explores from the reset entry and any extra entries (seeds).
    static Program analyze(const rom::RomSet& rom, const std::vector<Loc>& seeds = {});

    [[nodiscard]] const std::map<Loc, Node>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::set<Loc>& functions() const noexcept { return functions_; }
    [[nodiscard]] const std::set<Loc>& return_sites() const noexcept { return return_sites_; }
    [[nodiscard]] const std::vector<Finding>& findings() const noexcept { return findings_; }
    // Immediate values stored to the interrupt and trap vectors in RAM.
    [[nodiscard]] const std::map<std::uint16_t, std::set<std::uint16_t>>& vector_stores() const noexcept { return vectors_; }
    // Locations decoded only as operand words.
    [[nodiscard]] const std::set<Loc>& operand_words() const noexcept { return operand_words_; }
    // Entries found only as pointer immediates (MOV #addr,...), after they
    // passed validation; and those that didn't.
    [[nodiscard]] const std::set<Loc>& pointer_entries() const noexcept { return pointer_entries_; }
    [[nodiscard]] const std::set<std::uint16_t>& rejected_pointers() const noexcept { return rejected_pointers_; }
    // Register-save helpers (JSR Rn,@#helper; the helper continues its
    // caller with JSR PC,@(SP)+), and the functions found by their calls.
    [[nodiscard]] const std::set<std::uint16_t>& save_helpers() const noexcept { return save_helpers_; }
    [[nodiscard]] const std::set<Loc>& prologue_entries() const noexcept { return prologue_entries_; }
    // Entries listed in the pointer table each code bank starts with.
    [[nodiscard]] const std::set<Loc>& bank_table_entries() const noexcept { return bank_table_entries_; }
    // The window each bank runs in, from its absolute references to itself.
    [[nodiscard]] const std::map<unsigned, int>& bank_windows() const noexcept { return bank_windows_; }
    // Window addresses stored as pointers: code whose bank is set elsewhere.
    [[nodiscard]] const std::set<std::uint16_t>& window_pointers() const noexcept { return window_pointers_; }

private:
    using Work = std::vector<std::pair<Loc, Banks>>;
    void explore(Loc entry, Banks banks);
    void run(Work work);
    bool resolve_tables();
    bool seed_pointers();
    bool seed_prologues();
    bool seed_bank_tables();
    void find_bank_windows();
    bool plausible_code(Loc entry) const;
    void add(Loc loc, Banks banks, Work& work);
    std::optional<std::uint16_t> word(Loc loc, unsigned offset) const;

    const rom::RomSet* rom_ = nullptr;
    std::map<Loc, Node> nodes_;
    std::set<Loc> operand_words_;
    std::set<Loc> functions_;
    std::set<Loc> return_sites_;
    std::vector<Finding> findings_;
    std::set<std::pair<Issue, Loc>> reported_;
    std::map<std::uint16_t, std::set<std::uint16_t>> vectors_;
    struct Table {
        std::vector<std::uint16_t> targets;
        bool heuristic;
        bool operator==(const Table&) const = default;
    };
    std::map<Loc, Table> tables_;   // jump tables: targets, and whether proven
    std::set<std::uint16_t> pointer_immediates_;
    std::set<Loc> pointer_entries_;
    std::set<std::uint16_t> rejected_pointers_;
    std::set<std::uint16_t> window_pointers_;
    std::set<std::uint16_t> save_helpers_;
    std::set<Loc> prologue_entries_;
    std::set<Loc> bank_table_entries_;
    std::map<unsigned, int> bank_windows_;
};

[[nodiscard]] const char* describe(Issue issue) noexcept;

} // namespace qilin::cfg
