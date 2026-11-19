#include "cfg.hpp"

#include "system2.hpp"
#include "vrange.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <optional>

namespace qilin::cfg {
namespace {

using t11::Op;

constexpr std::uint16_t reset_pc = 0x8000;   // initial mode 0x36FF

// The T-11's fixed vectors: traps 004-034, CP interrupts 060-154 (octal).
constexpr bool is_vector(std::uint16_t address) noexcept {
    return address >= 004 && address <= 0154 && (address & 3) == 0;
}

bool writes_destination(Op op) noexcept {
    switch (op) {
    case Op::cmp: case Op::bit: case Op::tst: case Op::mtps:
        return false;
    default:
        return true;
    }
}

bool is_branch(Op op) noexcept {
    switch (op) {
    case Op::bne: case Op::beq: case Op::bge: case Op::blt: case Op::bgt: case Op::ble:
    case Op::bpl: case Op::bmi: case Op::bhi: case Op::blos: case Op::bvc: case Op::bvs: case Op::bcc: case Op::bcs:
        return true;
    default:
        return false;
    }
}

// The address an operand names, when it is fixed by the instruction alone:
// @#a, or a PC-relative address. 'after' is the address following the
// operand's extension word.
std::optional<std::uint16_t> fixed_address(const t11::Operand& op, std::uint16_t after) noexcept {
    if (op.absolute()) {
        return op.word;
    }
    if (op.relative()) {
        return static_cast<std::uint16_t>(after + op.word);
    }
    return std::nullopt;
}

} // namespace

std::set<std::uint32_t> Program::table_words() const {
    std::set<std::uint32_t> words = bank_table_words_;
    for (const auto& [loc, table] : tables_) {
        words.insert(table.slots.begin(), table.slots.end());
    }
    return words;
}

std::uint32_t Loc::physical() const noexcept {
    return sys2::rom_offset(pc, bank, bank);
}

std::expected<std::vector<Loc>, std::string> read_seeds(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::unexpected(std::format("can't read {}", path));
    std::vector<Loc> seeds;
    std::string line;
    for (int number = 1; std::getline(in, line); ++number) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.erase(line.begin());
        if (line.empty()) continue;
        unsigned pc = 0, bank = 0;
        const auto slash = line.find("/b");
        const char* begin = line.data();
        const char* end = line.data() + (slash == std::string::npos ? line.size() : slash);
        const auto parsed = std::from_chars(begin, end, pc, 16);
        bool ok = parsed.ec == std::errc{} && parsed.ptr == end && pc >= 0x4000 && pc <= 0xffff && (pc & 1) == 0;
        if (ok && slash != std::string::npos) {
            const auto b = std::from_chars(line.data() + slash + 2, line.data() + line.size(), bank, 10);
            ok = b.ec == std::errc{} && b.ptr == line.data() + line.size() && bank < 64 && pc < 0x8000;
        } else if (ok) {
            ok = pc >= 0x8000;   // a window address needs its bank
        }
        if (!ok) return std::unexpected(std::format("{}:{}: not a location: {}", path, number, line));
        seeds.push_back(Loc{static_cast<std::uint16_t>(pc), static_cast<std::uint8_t>(bank)});
    }
    return seeds;
}

std::string Loc::name() const {
    return pc >= 0x8000 ? std::format("{:04X}", pc) : std::format("{:04X}/b{}", pc, bank);
}

const char* describe(Issue issue) noexcept {
    switch (issue) {
    case Issue::computed_jump: return "computed jump";
    case Issue::computed_call: return "computed call";
    case Issue::unknown_bank: return "enters a window whose bank isn't known here";
    case Issue::odd_pc_mode: return "unusual PC addressing mode";
    case Issue::trap: return "trap instruction";
    case Issue::halt: return "HALT";
    case Issue::outside_rom: return "control leaves ROM";
    case Issue::missing_rom: return "no ROM at a code address";
    case Issue::overlap: return "code overlaps another instruction's operand";
    case Issue::byte_bank_write: return "byte write to a bank selector";
    }
    return "?";
}

std::optional<std::uint16_t> Program::word(Loc loc, unsigned offset) const {
    const auto address = static_cast<std::uint16_t>(loc.pc + offset);
    if (address < 0x4000 || (address >= 0x8000) != (loc.pc >= 0x8000) ||
        (address < 0x8000 && (address >= 0x6000) != (loc.pc >= 0x6000))) {
        return std::nullopt;   // an instruction never spans two windows or a window and fixed ROM
    }
    return rom_->word(rom::Region::main_cpu, sys2::rom_offset(address, loc.bank, loc.bank));
}

// Heuristic jump tables can read one entry too many: an address inside an
// instruction found later. Each such table is cut before that entry and the
// analysis runs again from scratch, until no heuristic table overlaps code.
Program Program::analyze(const rom::RomSet& rom, const std::vector<Loc>& seeds) {
    std::map<Loc, std::size_t> limits;
    for (int round = 0;; ++round) {
        auto p = analyze_once(rom, seeds, limits);
        bool cut = false;
        for (const auto& [instruction, inside] : p.overlaps_) {
            for (const auto& [loc, table] : p.tables_) {
                if (!table.heuristic) {
                    continue;
                }
                for (std::size_t k = 0; k < table.targets.size(); ++k) {
                    if (table.targets[k] == inside.pc || table.targets[k] == instruction.pc) {
                        const auto limit = limits.find(loc);
                        if (limit == limits.end() || limit->second > k) {
                            limits[loc] = k;
                            cut = true;
                        }
                        break;
                    }
                }
            }
        }
        if (!cut || round == 16) {
            return p;
        }
    }
}

Program Program::analyze_once(const rom::RomSet& rom, const std::vector<Loc>& seeds,
                              const std::map<Loc, std::size_t>& table_limits) {
    Program p;
    p.rom_ = &rom;
    p.table_limits_ = table_limits;
    p.functions_.insert(Loc{reset_pc, 0});
    p.explore(Loc{reset_pc, 0}, Banks{{0, 0}});   // MAME's bank entries start at 0
    for (const auto& seed : seeds) {
        p.functions_.insert(seed);
        p.explore(seed, Banks{});
    }
    // Vector handlers the game installs with an immediate store (in the fixed
    // ROM they are unambiguous entries), and jump tables value-range analysis
    // proves the size of, until neither adds anything.
    p.find_bank_windows();
    for (bool grew = true; grew;) {
        grew = p.resolve_tables();
        grew = p.seed_pointers() || grew;
        grew = p.seed_prologues() || grew;
        grew = p.seed_bank_tables() || grew;
        if (!grew) grew = p.seed_data_pointers();   // last: only once nothing better is left
        if (!grew) grew = p.seed_gaps();            // and last of all, what nothing reaches
        for (const auto& [vector, handlers] : std::map(p.vectors_)) {
            for (const auto handler : handlers) {
                const Loc entry{handler, 0};
                if (handler >= 0x8000 && (handler & 1) == 0 && p.functions_.insert(entry).second) {
                    p.explore(entry, Banks{});
                    grew = true;
                }
            }
        }
    }
    return p;
}

void Program::explore(Loc entry, Banks entry_banks) {
    // Code running from a window has that window's bank, whatever else is unknown.
    if (entry.window() >= 0 && entry_banks.b[entry.window()] == Banks::unknown) {
        entry_banks.b[entry.window()] = entry.bank;
    }
    Work work;
    add(entry, entry_banks, work);
    run(std::move(work));
}

void Program::run(Work work) {
    while (!work.empty()) {
        auto [loc, in] = work.back();
        work.pop_back();
        const auto report = [&](Issue issue, std::string detail) {
            if (reported_.emplace(issue, loc).second) {
                findings_.push_back({issue, loc, std::move(detail)});
            }
        };

        if (loc.pc < 0x4000 || (loc.pc & 1) != 0) {
            report(Issue::outside_rom, std::format("control reaches {:04X}", loc.pc));
            nodes_.erase(loc);
            continue;
        }
        const auto w0 = word(loc, 0);
        if (!w0) {
            report(Issue::missing_rom, std::format("physical offset {:05X}", loc.physical()));
            nodes_.erase(loc);
            continue;
        }
        if (operand_words_.contains(loc)) {
            report(Issue::overlap, "a branch lands inside another instruction");
            for (const Loc start : {Loc{static_cast<std::uint16_t>(loc.pc - 2), loc.bank},
                                    Loc{static_cast<std::uint16_t>(loc.pc - 4), loc.bank}}) {
                if (nodes_.contains(start)) overlaps_.emplace_back(start, loc);
            }
        }
        auto& node = nodes_[loc];
        node.insn = t11::decode(*w0, word(loc, 2).value_or(0), word(loc, 4).value_or(0));
        node.banks_in = in;
        const auto& insn = node.insn;
        const auto text = t11::disassemble(insn, loc.pc);
        if (insn.legal) {
            for (unsigned i = 1; i < insn.words; ++i) {
                const Loc operand{static_cast<std::uint16_t>(loc.pc + 2 * i), loc.bank};
                if (!word(loc, 2 * i)) {
                    report(Issue::missing_rom, "an operand word is missing");
                }
                if (nodes_.contains(operand)) {
                    report(Issue::overlap, std::format("operand word at {} is also an instruction", operand.name()));
                    overlaps_.emplace_back(loc, operand);
                }
                operand_words_.insert(operand);
            }
        }

        const auto next_pc = insn.legal ? insn.next(loc.pc) : static_cast<std::uint16_t>(loc.pc + 2);
        Banks out = in;
        // Where control goes to 'target' with the banks 'out': a location, or
        // nothing if the target's window bank isn't known.
        const auto locate = [&](std::uint16_t target) -> std::optional<Loc> {
            if (target >= 0x8000) {
                return Loc{target, 0};
            }
            if (target < 0x4000) {
                report(Issue::outside_rom, std::format("{} goes to {:04X}", text, target));
                return std::nullopt;
            }
            const int w = target >= 0x6000 ? 1 : 0;
            if (out.b[w] == Banks::unknown) {
                node.window_entry = w;
                report(Issue::unknown_bank, std::format("{} -> {:04X} in window {}", text, target, w));
                return std::nullopt;
            }
            return Loc{target, static_cast<std::uint8_t>(out.b[w])};
        };

        // Operands with PC in modes compilers never use.
        for (const auto* op : {&insn.src, &insn.dst}) {
            if (op->reg == 7 && (op->mode == 1 || op->mode == 4 || op->mode == 5) &&
                (op == &insn.dst || insn.operands == 2)) {
                report(Issue::odd_pc_mode, text);
            }
        }

        // Bank selector writes: MOV #n, CLR and MOV Rn to @#1400-147F.
        if (insn.legal && insn.operands >= 1 && insn.dst.absolute() && writes_destination(insn.op) &&
            sys2::area(insn.dst.word) == sys2::Area::adc) {
            node.writes_bank = true;
            const int w = (insn.dst.word >> 1) & 1;
            if (insn.byte) {
                report(Issue::byte_bank_write, text);
                out.b[w] = Banks::unknown;
            } else if (insn.op == Op::mov && insn.src.immediate()) {
                out.b[w] = sys2::select_rom_bank(insn.src.word);
            } else if (insn.op == Op::clr) {
                out.b[w] = sys2::select_rom_bank(0);
            } else {
                out.b[w] = Banks::unknown;
            }
        }
        // Pointer immediates: MOV #addr to a register or memory.
        // A window address written by code running in that same window
        // points into the code's own bank (APB's bank-12 code fills its RAM
        // handler tables this way); one into the other window can't be placed.
        if (insn.legal && insn.op == Op::mov && !insn.byte && insn.src.immediate() && (insn.src.word & 1) == 0) {
            const Loc pointer{insn.src.word, 0};
            if (insn.src.word >= 0x8000) {
                pointer_immediates_.insert(pointer);
            } else if (insn.src.word >= 0x4000 && pointer.window() == loc.window()) {
                pointer_immediates_.insert(Loc{insn.src.word, loc.bank});
            } else if (insn.src.word >= 0x4000) {
                window_pointers_.insert(insn.src.word);
            }
        }
        // Immediate stores to the vectors.
        if (insn.legal && insn.op == Op::mov && !insn.byte && insn.src.immediate() && insn.dst.absolute() &&
            is_vector(insn.dst.word)) {
            vectors_[insn.dst.word].insert(insn.src.word);
        }

        std::vector<Loc> next;
        std::vector<Loc> callees;   // explored, but not successors within the caller
        const auto fall_through = [&] {
            if (const auto l = locate(next_pc); l) {
                next.push_back(*l);
            }
        };

        if (!insn.legal) {
            node.flow = Flow::trap;
            report(Issue::trap, std::format("{} traps through {:03o}", text, t11::trap_vector(insn)));
        } else {
            switch (insn.op) {
            case Op::halt:
                node.flow = Flow::halt;
                report(Issue::halt, text);
                break;
            case Op::wait:
                node.flow = Flow::wait;
                fall_through();
                break;
            case Op::rti:
            case Op::rtt:
                node.flow = Flow::rti;
                break;
            case Op::rts:
                node.flow = Flow::ret;
                break;
            case Op::bpt: case Op::iot: case Op::emt: case Op::trap:
                node.flow = Flow::trap;
                report(Issue::trap, std::format("{} traps through {:03o}", text, t11::trap_vector(insn)));
                fall_through();
                break;
            case Op::br:
            case Op::sob:
                if (insn.op == Op::sob && insn.reg == 7) {
                    report(Issue::odd_pc_mode, text);
                }
                node.flow = insn.op == Op::br ? Flow::jump : Flow::branch;
                if (const auto t = locate(insn.branch_target(loc.pc)); t) {
                    node.target = t;
                    next.push_back(*t);
                }
                if (insn.op == Op::sob) {
                    fall_through();
                }
                break;
            case Op::jmp:
            case Op::jsr: {
                const bool call = insn.op == Op::jsr;
                const auto target = fixed_address(insn.dst, next_pc);
                if (const auto table = tables_.find(loc); !call && table != tables_.end()) {
                    // JMP @X(Rn) through a table resolve_tables sized.
                    node.flow = Flow::table;
                    node.table = table->second.targets;
                    node.heuristic_table = table->second.heuristic;
                    for (const auto entry : table->second.targets) {
                        if (const auto t = locate(entry); t) {
                            next.push_back(*t);
                        }
                    }
                } else if (target) {
                    node.flow = call ? Flow::call : Flow::jump;
                    if (const auto t = locate(*target); t) {
                        node.target = t;
                        (call ? callees : next).push_back(*t);
                        if (call) {
                            functions_.insert(*t);
                        }
                    }
                } else {
                    node.flow = call ? Flow::call_dynamic : Flow::jump_dynamic;
                    report(call ? Issue::computed_call : Issue::computed_jump, text);
                }
                if (call) {
                    // The callee may change the banks before it returns. The
                    // caller's own window keeps its bank, or the return is a
                    // mismatch the runtime resolves.
                    out = Banks{};
                    if (loc.window() >= 0) {
                        out.b[loc.window()] = loc.bank;
                    }
                    const Loc site{next_pc, loc.window() >= 0 ? loc.bank : std::uint8_t{0}};
                    if (next_pc >= 0x4000) {
                        return_sites_.insert(site);
                        next.push_back(site);
                    }
                }
                break;
            }
            default:
                if (const auto table = tables_.find(loc); table != tables_.end()) {
                    node.flow = Flow::table;
                    node.table = table->second.targets;
                    node.heuristic_table = table->second.heuristic;
                    for (const auto entry : table->second.targets) {
                        if (const auto t = locate(entry); t) {
                            next.push_back(*t);
                        }
                    }
                } else if (is_branch(insn.op)) {
                    node.flow = Flow::branch;
                    if (const auto t = locate(insn.branch_target(loc.pc)); t) {
                        node.target = t;
                        next.push_back(*t);
                    }
                    fall_through();
                } else if (insn.operands >= 1 && insn.dst.reg == 7 && insn.dst.mode == 0 && writes_destination(insn.op)) {
                    node.flow = Flow::jump_dynamic;
                    report(Issue::computed_jump, text);
                } else if (insn.op == Op::xor_ && insn.reg == 7) {
                    fall_through();   // XOR PC,dst reads PC: harmless
                } else {
                    // A write to our own window's selector fetches the next
                    // instruction from the new bank.
                    if (node.writes_bank && loc.window() >= 0) {
                        const int w = loc.window();
                        if (out.b[w] != loc.bank) {
                            if (out.b[w] == Banks::unknown) {
                                node.window_entry = w;
                                report(Issue::unknown_bank, text + " switches the bank it runs from");
                            } else {
                                next.push_back(Loc{next_pc, static_cast<std::uint8_t>(out.b[w])});
                            }
                            break;
                        }
                    }
                    fall_through();
                }
                break;
            }
        }
        node.next = next;
        next.insert(next.end(), callees.begin(), callees.end());
        for (const auto& successor : next) {
            Banks banks = out;
            if (successor.window() >= 0) {
                banks.b[successor.window()] = successor.bank;
            }
            add(successor, banks, work);
        }
    }
}

// Sizes the computed jumps that index a ROM table, ADD X(Rn),PC (offsets
// from the next instruction) and JMP @X(Rn) (addresses), from the range
// value-range analysis proves for Rn. Re-explores each table it proves.
bool Program::resolve_tables() {
    const auto ranges = vrange::analyze(*this);
    Work work;
    for (const auto& [loc, node] : nodes_) {
        const auto& insn = node.insn;
        if (node.flow != Flow::jump_dynamic && node.flow != Flow::table) {
            continue;
        }
        const bool add_pc = insn.op == Op::add && insn.dst.mode == 0 && insn.dst.reg == 7;
        const bool jmp_indexed = insn.op == Op::jmp && insn.dst.mode == t11::index_deferred;
        const auto& index = add_pc ? insn.src : insn.dst;
        if ((!add_pc && !jmp_indexed) || index.mode != (add_pc ? t11::index : t11::index_deferred) || index.reg > 5) {
            continue;
        }
        const auto range_at = ranges.find(loc);
        if (range_at == ranges.end()) {
            continue;
        }
        const auto known = tables_.find(loc);
        // The index range: proven, or (heuristic) from 0 or the proven
        // lower bound until the first entry that doesn't hold up.
        const auto range = range_at->second[index.reg];
        // A proof counts only if every slot in the range holds plausible code;
        // otherwise (a range wider than the table, such as a byte index from
        // RAM) the entries are read until the first that isn't, and the
        // lifted code checks the index at run time.
        bool proven = !range.top() && range.size() <= 2 * 256;
        const std::int32_t first = range.top() ? 0 : range.lo;
        const std::int32_t last = range.top() ? 2 * 255 : std::min(range.hi, range.lo + 2 * 255);
        const auto after = insn.next(loc.pc);
        Table table{{}, false, {}};
        std::set<std::uint16_t> seen;
        const auto limit = table_limits_.find(loc);
        for (std::int32_t r = first; r <= last; ++r) {
            if (limit != table_limits_.end() && table.targets.size() >= limit->second) {
                proven = false;
                break;
            }
            const auto slot = static_cast<std::uint16_t>((index.word + r) & 0xfffe);
            if (!seen.insert(slot).second) {
                continue;
            }
            std::optional<std::uint16_t> entry;
            const int w = slot >= 0x6000 ? 1 : 0;
            if (slot >= 0x8000 || (slot >= 0x4000 && node.banks_in.b[w] != Banks::unknown)) {
                const auto bank = static_cast<std::uint8_t>(slot >= 0x8000 ? 0 : node.banks_in.b[w]);
                entry = rom_->word(rom::Region::main_cpu, sys2::rom_offset(slot, bank, bank));
            }
            const auto target = entry ? static_cast<std::uint16_t>(add_pc ? after + *entry : *entry) : std::uint16_t{0};
            const bool plausible = entry && (target & 1) == 0 && target >= 0x4000 && [&] {
                const int tw = target >= 0x8000 ? -1 : (target >= 0x6000 ? 1 : 0);
                if (tw >= 0 && node.banks_in.b[tw] == Banks::unknown) return false;
                const Loc t{target, static_cast<std::uint8_t>(tw < 0 ? 0 : node.banks_in.b[tw])};
                return !operand_words_.contains(t) && (nodes_.contains(t) || plausible_code(t));
            }();
            if (!plausible) {
                proven = false;
                break;
            }
            table.targets.push_back(target);
            table.slots.push_back(sys2::rom_offset(slot, static_cast<std::uint8_t>(slot >= 0x8000 ? 0 : node.banks_in.b[w]),
                                                   static_cast<std::uint8_t>(slot >= 0x8000 ? 0 : node.banks_in.b[w])));
        }
        table.heuristic = !proven;
        if (table.targets.empty()) {
            if (known != tables_.end()) {
                tables_.erase(known);
                work.emplace_back(loc, node.banks_in);
            }
            continue;
        }
        if (known == tables_.end() || known->second != table) {
            tables_[loc] = table;
            work.emplace_back(loc, node.banks_in);
            std::erase_if(findings_, [&](const Finding& f) { return f.at == loc && f.issue == Issue::computed_jump; });
            reported_.erase({Issue::computed_jump, loc});
        }
    }
    const bool grew = !work.empty();
    run(std::move(work));
    return grew;
}

// A pointer immediate is weak evidence of code: a constant can look like an
// address. It becomes an entry only if straight-line decoding from it reaches
// an instruction that ends the run (return, jump, branch always) through
// instructions the T-11 executes, with no PC addressing a compiler or
// assembler wouldn't write, inside the ROM.
bool Program::plausible_code(Loc entry, bool strict) const {
    Loc at = entry;
    std::optional<Op> last_branch;
    for (int count = 0; count < 64; ++count) {
        const auto w0 = word(at, 0);
        if (!w0 || operand_words_.contains(at)) {
            return false;
        }
        const auto insn = t11::decode(*w0, word(at, 2).value_or(0), word(at, 4).value_or(0));
        if (!insn.legal || insn.op == Op::halt || insn.op == Op::wait || insn.op == Op::reset) {
            return false;
        }
        // A value only guessed to be a pointer must look like ordinary code:
        // no traps or RTT, which data decodes to far more often than code uses.
        if (strict && (insn.op == Op::bpt || insn.op == Op::iot || insn.op == Op::emt || insn.op == Op::trap ||
                       insn.op == Op::rtt)) {
            return false;
        }
        // Nor a JMP or JSR to -(Rn): legal, and never written.
        if (strict && (insn.op == Op::jmp || insn.op == Op::jsr) && insn.dst.mode == t11::autodec) {
            return false;
        }
        // Nor two conditional branches on the same condition in a row: the
        // second could never be taken. Tables of small words decode that way.
        if (strict && is_branch(insn.op) && last_branch == insn.op) {
            return false;
        }
        last_branch = is_branch(insn.op) ? std::optional<Op>(insn.op) : std::nullopt;
        for (unsigned i = 1; i < insn.words; ++i) {
            if (!word(at, 2 * i)) {
                return false;
            }
        }
        for (const auto* op : {&insn.src, &insn.dst}) {
            if (op->reg == 7 && (op->mode == 1 || op->mode == 4 || op->mode == 5)) {
                return false;
            }
        }
        if (nodes_.contains(at) && at != entry) {
            return true;   // joins code already proven
        }
        switch (insn.op) {
        case Op::rts: case Op::rti: case Op::rtt: case Op::jmp: case Op::br:
            return true;
        default:
            break;
        }
        if (insn.operands >= 1 && insn.dst.mode == 0 && insn.dst.reg == 7 && writes_destination(insn.op)) {
            return true;
        }
        at.pc = insn.next(at.pc);
        if (at.pc < 0x8000 && entry.pc >= 0x8000) {
            return false;
        }
    }
    return false;
}

bool Program::seed_pointers() {
    bool grew = false;
    for (const auto entry : std::set(pointer_immediates_)) {
        if (functions_.contains(entry) || rejected_pointers_.contains(entry)) {
            continue;
        }
        if (nodes_.contains(entry) || plausible_code(entry, true)) {
            functions_.insert(entry);
            pointer_entries_.insert(entry);
            explore(entry, Banks{});
            grew = true;
        } else {
            rejected_pointers_.insert(entry);
        }
    }
    return grew;
}

// Banked code runs in one window: the one its absolute JSR, JMP and pointer
// stores refer to. A bank with no such references is taken to hold data.
void Program::find_bank_windows() {
    for (unsigned bank = 0; bank < sys2::rom_bank_count; ++bank) {
        std::array<unsigned, 2> refs{};
        const std::uint32_t base = 0x10000 + bank * 0x2000;
        for (std::uint32_t offset = 0; offset + 2 < 0x2000; offset += 2) {
            const auto op = rom_->word(rom::Region::main_cpu, base + offset);
            const auto operand = rom_->word(rom::Region::main_cpu, base + offset + 2);
            if (!op || !operand) {
                continue;
            }
            // JSR PC,@#a; JMP @#a; and pointers loaded as immediates: MOV #a,@#x and MOV #a,Rn.
            const bool absolute_transfer = *op == 004737 || *op == 000137 || *op == 012737 || (*op & 0177770) == 012700;
            if (absolute_transfer && *operand >= 0x4000 && *operand < 0x8000) {
                ++refs[*operand >= 0x6000 ? 1 : 0];
            }
        }
        if (refs[0] + refs[1] >= 4) {
            bank_windows_[bank] = refs[1] > refs[0] ? 1 : 0;
        }
    }
}

// Functions that open by calling a register-save helper. A helper is a
// JSR Rn,@#H target (n = 0-5) that continues its caller with the coroutine
// call JSR PC,@(SP)+ within its first few instructions; a call to one is an
// unambiguous function prologue, found in the fixed ROM and in every bank
// in the window it runs in.
bool Program::seed_prologues() {
    for (const auto& [loc, node] : nodes_) {
        const auto& insn = node.insn;
        if (node.flow != Flow::call || insn.reg >= 6 || !node.target || node.target->pc < 0x8000) {
            continue;
        }
        Loc at = *node.target;
        for (int i = 0; i < 8; ++i) {
            const auto found = nodes_.find(at);
            if (found == nodes_.end()) {
                break;
            }
            const auto& h = found->second.insn;
            if (h.op == Op::jsr && h.reg == 7 && h.dst.mode == t11::autoinc_deferred && h.dst.reg == 6) {
                save_helpers_.insert(node.target->pc);
                break;
            }
            if (found->second.flow != Flow::next) {
                break;
            }
            at.pc = h.next(at.pc);
        }
    }
    if (save_helpers_.empty()) {
        return false;
    }
    std::vector<Loc> found;
    const auto scan = [&](std::uint32_t physical, std::uint32_t size, std::uint16_t cpu_base, std::uint8_t bank) {
        for (std::uint32_t offset = 0; offset + 2 < size; offset += 2) {
            const auto op = rom_->word(rom::Region::main_cpu, physical + offset);
            const auto target = rom_->word(rom::Region::main_cpu, physical + offset + 2);
            if (op && target && (*op & 0177077) == 004037 && ((*op >> 6) & 7) < 6 &&
                save_helpers_.contains(*target)) {
                found.push_back(Loc{static_cast<std::uint16_t>(cpu_base + offset), bank});
            }
        }
    };
    scan(0x8000, 0x8000, 0x8000, 0);
    for (const auto& [bank, window] : bank_windows_) {
        scan(0x10000 + bank * 0x2000, 0x2000, static_cast<std::uint16_t>(window == 0 ? 0x4000 : 0x6000),
             static_cast<std::uint8_t>(bank));
    }
    bool grew = false;
    for (const auto& entry : found) {
        if (functions_.contains(entry) || operand_words_.contains(entry)) {
            continue;
        }
        functions_.insert(entry);
        prologue_entries_.insert(entry);
        Banks banks;
        if (entry.window() >= 0) {
            banks.b[entry.window()] = entry.bank;
        }
        explore(entry, banks);
        grew = true;
    }
    return grew;
}

// Each code bank starts with a table of entry points into itself (unused
// slots are 0); code in the fixed ROM jumps through its slots (JMP @#40xx)
// and calls through it (JSR PC,@40000(Rn)). The table ends at the first word
// that isn't an even address in the bank's window, and can't run past the
// lowest address it lists. Each entry is checked like a pointer immediate.
bool Program::seed_bank_tables() {
    bool grew = false;
    for (const auto& [bank, window] : bank_windows_) {
        const std::uint16_t base = window == 0 ? 0x4000 : 0x6000;
        const std::uint32_t physical = 0x10000 + bank * 0x2000;
        std::uint32_t end = 0x2000;
        std::vector<std::uint16_t> entries;
        for (std::uint32_t offset = 0; offset < end; offset += 2) {
            const auto value = rom_->word(rom::Region::main_cpu, physical + offset);
            if (!value || (*value != 0 && ((*value & 1) != 0 || *value < base || *value >= base + 0x2000))) {
                break;
            }
            if (*value != 0) {
                entries.push_back(*value);
                end = std::min<std::uint32_t>(end, *value - base);
            }
        }
        for (std::uint32_t offset = 0; offset < end && offset < 0x2000; offset += 2) {
            bank_table_words_.insert(physical + offset);
        }
        for (const auto address : entries) {
            const Loc entry{address, static_cast<std::uint8_t>(bank)};
            if (functions_.contains(entry)) {
                continue;
            }
            if (!nodes_.contains(entry) && !plausible_code(entry)) {
                continue;
            }
            functions_.insert(entry);
            bank_table_entries_.insert(entry);
            Banks banks;
            banks.b[window] = static_cast<int>(bank);
            explore(entry, banks);
            grew = true;
        }
    }
    return grew;
}

// Code pointers kept in ROM data: records and call tables the program calls
// through (CALL @X(Rn) with Rn an offset into records, handler lists copied
// to RAM). A word outside known code whose value is an address in the
// fixed ROM, or in its own bank's window, that isn't known code yet and
// decodes like real code (strictly: no traps, RTT or repeated branches,
// joining known code or ending a path) is taken as an entry. Data whose
// values happen to land on code only adds lifted entries nobody calls.
// Whether exploring from entry adds only clean code: everything it reaches
// that wasn't known code decodes without traps, halts, odd PC modes, leaving
// ROM, or computed jumps other than the table forms resolve_tables sizes
// (ADD X(Rn),PC and JMP @X(Rn) with X in ROM). Data that decodes well for a
// few instructions rarely survives that. Landing inside another instruction
// is allowed: real code does it (a skip over an operand), and APB's does it
// in code only data pointers reach.
bool Program::explores_cleanly(Loc entry, Banks banks) const {
    Program trial = *this;
    const std::size_t before = trial.findings_.size();
    trial.functions_.insert(entry);
    trial.explore(entry, banks);
    const auto table_form = [&](const Finding& f) {
        const auto n = trial.nodes_.find(f.at);
        if (n == trial.nodes_.end()) return false;
        const auto& insn = n->second.insn;
        const bool add_pc = insn.op == Op::add && insn.dst.mode == 0 && insn.dst.reg == 7 && insn.src.mode == t11::index &&
                            insn.src.reg <= 5 && insn.src.word >= 0x4000;
        const bool jmp_indexed = insn.op == Op::jmp && insn.dst.mode == t11::index_deferred && insn.dst.reg <= 5 &&
                                 insn.dst.word >= 0x4000;
        return add_pc || jmp_indexed;
    };
    return std::none_of(trial.findings_.begin() + static_cast<std::ptrdiff_t>(before), trial.findings_.end(),
                        [&](const Finding& f) {
                            switch (f.issue) {
                            case Issue::computed_call: case Issue::unknown_bank: case Issue::byte_bank_write:
                            case Issue::overlap:
                                return false;
                            case Issue::computed_jump:
                                return !table_form(f);
                            default:
                                return true;
                            }
                        }) &&
           std::all_of(trial.nodes_.begin(), trial.nodes_.end(), [&](const auto& n) {
               return nodes_.contains(n.first) || (n.second.insn.legal && n.second.flow != Flow::trap &&
                                                   n.second.flow != Flow::halt && n.second.flow != Flow::rti);
           });
}

// Gaps: stretches of the fixed ROM and the code banks that are neither code
// nor tables after everything else, whose first word decodes strictly like
// code and explores cleanly, are lifted too, as speculative entries. Code
// nothing found a way into (unused, or reached in ways no analysis or run
// has seen) is then there if control ever arrives at its start; data that
// passes only adds lifted code nobody runs.
bool Program::seed_gaps() {
    std::set<std::uint32_t> taken = table_words();
    for (const auto& [loc, node] : nodes_) {
        const unsigned n = node.insn.legal ? node.insn.words : 1u;
        for (unsigned i = 0; i < n; ++i) taken.insert(Loc{static_cast<std::uint16_t>(loc.pc + 2 * i), loc.bank}.physical());
    }
    std::vector<std::pair<Loc, int>> starts;
    const auto scan = [&](std::uint32_t physical, std::uint32_t size, std::uint16_t cpu_base, int window, std::uint8_t bank) {
        bool previous_taken = true;
        for (std::uint32_t offset = 0; offset < size; offset += 2) {
            const bool now_taken = taken.contains(physical + offset);
            if (!now_taken && previous_taken && rom_->word(rom::Region::main_cpu, physical + offset)) {
                starts.emplace_back(Loc{static_cast<std::uint16_t>(cpu_base + offset), bank}, window);
            }
            previous_taken = now_taken;
        }
    };
    scan(0x8000, 0x8000, 0x8000, -1, 0);
    for (const auto& [bank, window] : bank_windows_) {
        scan(0x10000 + bank * 0x2000, 0x2000, static_cast<std::uint16_t>(window == 1 ? 0x6000 : 0x4000), window,
             static_cast<std::uint8_t>(bank));
    }
    bool grew = false;
    for (const auto& [entry, window] : starts) {
        if (nodes_.contains(entry) || operand_words_.contains(entry) || rejected_gaps_.contains(entry)) continue;
        Banks banks;
        if (window >= 0) banks.b[window] = entry.bank;
        if (!plausible_code(entry, true) || !explores_cleanly(entry, banks)) {
            rejected_gaps_.insert(entry);
            continue;
        }
        functions_.insert(entry);
        gap_entries_.insert(entry);
        explore(entry, banks);
        grew = true;
    }
    return grew;
}

bool Program::seed_data_pointers() {
    std::set<std::uint32_t> code_words;
    for (const auto& [loc, node] : nodes_) {
        const unsigned n = node.insn.legal ? node.insn.words : 1u;
        for (unsigned i = 0; i < n; ++i) code_words.insert(Loc{static_cast<std::uint16_t>(loc.pc + 2 * i), loc.bank}.physical());
    }
    std::vector<std::pair<Loc, int>> found;   // entry, window (-1 fixed)
    const auto scan = [&](std::uint32_t physical, std::uint32_t size, int window, std::uint8_t bank) {
        const std::uint16_t base = window == 1 ? 0x6000 : 0x4000;
        for (std::uint32_t offset = 0; offset < size; offset += 2) {
            if (code_words.contains(physical + offset)) continue;
            const auto value = rom_->word(rom::Region::main_cpu, physical + offset);
            if (!value || (*value & 1)) continue;
            Loc entry;
            if (*value >= 0x8000) {
                entry = Loc{*value, 0};
            } else if (window >= 0 && *value >= base && *value < base + 0x2000) {
                entry = Loc{*value, bank};
            } else {
                continue;
            }
            if (nodes_.contains(entry) || operand_words_.contains(entry) || code_words.contains(entry.physical()) ||
                bank_table_words_.contains(entry.physical()) || rejected_data_pointers_.contains(entry)) {
                continue;
            }
            found.emplace_back(entry, entry.pc >= 0x8000 ? -1 : window);
        }
    };
    scan(0x8000, 0x8000, -1, 0);
    for (unsigned bank = 0; bank < sys2::rom_bank_count; ++bank) {
        // In a data bank only pointers into the fixed ROM say where they go.
        const auto window = bank_windows_.find(bank);
        scan(0x10000 + bank * 0x2000, 0x2000, window == bank_windows_.end() ? -1 : window->second, static_cast<std::uint8_t>(bank));
    }
    // Lowest address first: of a function's start and a pointer into the
    // middle of one of its instructions, the start is taken and the other
    // is then an operand word.
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    bool grew = false;
    for (const auto& [entry, window] : found) {
        // Entries accepted earlier in this pass may have made this one an operand word.
        if (nodes_.contains(entry) || functions_.contains(entry) || rejected_data_pointers_.contains(entry) ||
            operand_words_.contains(entry)) {
            continue;
        }
        if (!plausible_code(entry, true)) {
            rejected_data_pointers_.insert(entry);
            continue;
        }
        Banks banks;
        if (window >= 0) {
            banks.b[window] = entry.bank;
        }
        const bool clean = explores_cleanly(entry, banks);
        if (!clean) {
            rejected_data_pointers_.insert(entry);
            continue;
        }
        functions_.insert(entry);
        data_pointer_entries_.insert(entry);
        explore(entry, banks);
        grew = true;
    }
    return grew;
}

// Queues a location, or widens what is known about its banks and queues it
// again; a location is revisited only when its bank state loses information.
void Program::add(Loc loc, Banks banks, Work& work) {
    const auto found = nodes_.find(loc);
    if (found == nodes_.end()) {
        nodes_[loc].banks_in = banks;
        work.emplace_back(loc, banks);
        return;
    }
    Banks merged = found->second.banks_in;
    for (int w = 0; w < 2; ++w) {
        if (merged.b[w] != banks.b[w]) {
            merged.b[w] = Banks::unknown;
        }
    }
    if (merged != found->second.banks_in) {
        found->second.banks_in = merged;
        work.emplace_back(loc, merged);
    }
}

} // namespace qilin::cfg
