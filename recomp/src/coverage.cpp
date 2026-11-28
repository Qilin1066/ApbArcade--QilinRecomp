#include "coverage.hpp"

#include "m6502_cfg.hpp"
#include "system2.hpp"
#include "vrange.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <map>
#include <print>
#include <set>

namespace qilin::coverage {

namespace {

enum Class : std::uint8_t { unknown, code, table, data, fill, data_bank };
constexpr const char* class_name[] = {"unknown", "code", "table", "data", "fill", "data bank"};

struct Totals {
    std::size_t bytes[6] = {};
    std::size_t candidates = 0, candidate_bytes = 0;
};

// One ROM area, word by word (T-11) or byte by byte (6502).
struct Area {
    std::string name;
    std::uint32_t first = 0, size = 0;    // physical offset and length in units
    std::uint16_t cpu_base = 0;           // the CPU address of its first unit
    std::uint8_t bank = 0;
    bool code_bank = true;
    [[nodiscard]] bool bank_first() const noexcept { return first >= 0x10000 / 2; }   // a bank, not the fixed ROM
};

std::string percent(std::size_t part, std::size_t whole) {
    return whole ? std::format("{:5.1f}%", 100.0 * static_cast<double>(part) / static_cast<double>(whole)) : "     -";
}

void print_header() {
    std::print("{:<16} {:>8} {:>8} {:>8} {:>8} {:>10} {:>8}  {:>9}  {}\n", "area", "code", "table", "data", "fill", "data bank",
               "unknown", "explained", "code-like unknown");
}

void print_row(const std::string& name, const Totals& t) {
    std::size_t total = 0;
    for (const auto b : t.bytes) total += b;
    std::print("{:<16} {:>8} {:>8} {:>8} {:>8} {:>10} {:>8}  {:>9}  {}\n", name, t.bytes[code], t.bytes[table], t.bytes[data],
               t.bytes[fill], t.bytes[data_bank], t.bytes[unknown], percent(total - t.bytes[unknown], total),
               t.candidates ? std::format("{} stretches, {} bytes", t.candidates, t.candidate_bytes) : std::string("none"));
}

// ---- T-11 -------------------------------------------------------------------

int main_cpu(const rom::RomSet& set, const Options& options, const cfg::Program& program) {
    const auto rom = set.region(rom::Region::main_cpu);
    const std::uint32_t words = static_cast<std::uint32_t>(rom.size() / 2);
    std::vector<std::uint8_t> cls(words, unknown);
    std::vector<bool> referenced(words, false);
    auto word_at = [&](std::uint32_t physical) { return set.word(rom::Region::main_cpu, physical); };

    for (const auto& [loc, node] : program.nodes()) {
        const unsigned n = node.insn.legal ? node.insn.words : 1u;
        for (unsigned i = 0; i < n; ++i) {
            const auto p = cfg::Loc{static_cast<std::uint16_t>(loc.pc + 2 * i), loc.bank}.physical() / 2;
            if (p < words) cls[p] = code;
        }
    }
    for (const auto p : program.table_words())
        if (p / 2 < words && cls[p / 2] == unknown) cls[p / 2] = table;

    // References from code to ROM: immediates, absolute and relative
    // addresses, and index bases, in the window's bank where it's known.
    for (const auto& [loc, node] : program.nodes()) {
        if (!node.insn.legal) continue;
        const std::uint16_t after = node.insn.next(loc.pc);
        for (const auto* op : {&node.insn.src, &node.insn.dst}) {
            if (!op->has_word()) continue;
            std::uint16_t address = op->word;
            if (op->relative() || op->relative_deferred()) address = static_cast<std::uint16_t>(after + op->word);
            std::uint32_t physical = 0;
            if (address >= 0x8000) {
                physical = address;
            } else if (address >= 0x4000) {
                const int w = address >= 0x6000 ? 1 : 0;
                int bank = node.banks_in.b[w];
                if (bank == cfg::Banks::unknown && loc.window() == w) bank = loc.bank;
                if (bank == cfg::Banks::unknown) continue;
                physical = sys2::rom_offset(address, static_cast<std::uint8_t>(bank), static_cast<std::uint8_t>(bank));
            } else {
                continue;
            }
            if (physical / 2 < words) referenced[physical / 2] = true;
        }
    }

    std::vector<Area> areas;
    areas.push_back({"fixed ROM", 0x8000 / 2, 0x8000 / 2, 0x8000, 0, true});
    for (unsigned bank = 0; bank < sys2::rom_bank_count; ++bank) {
        const std::uint32_t first = (0x10000 + bank * 0x2000) / 2;
        if (first + 0x1000 > words || !word_at(first * 2)) continue;
        const auto window = program.bank_windows().find(bank);
        const bool code_bank = window != program.bank_windows().end();
        areas.push_back({std::format("bank {:2}{}", bank, code_bank ? std::format(" (w{})", window->second) : ""), first, 0x1000,
                         static_cast<std::uint16_t>(code_bank && window->second == 1 ? 0x6000 : 0x4000),
                         static_cast<std::uint8_t>(bank), code_bank});
    }

    // Data banks first; then data from each reference to the next code or
    // table word; then, until nothing changes, data pointed to by data: a
    // word of data, a table or a data bank whose value is an address in the
    // fixed ROM, or in its own code bank's window, makes what it points to
    // data too (strings and records reached through pointer tables).
    std::vector<int> area_of(words, -1);
    for (std::size_t a = 0; a < areas.size(); ++a)
        for (std::uint32_t p = areas[a].first; p < areas[a].first + areas[a].size && p < words; ++p) area_of[p] = static_cast<int>(a);
    for (const auto& area : areas) {
        if (area.code_bank) continue;
        for (std::uint32_t p = area.first; p < area.first + area.size; ++p)
            if (cls[p] == unknown) cls[p] = data_bank;
    }
    // Indexed reads of ROM tables, X(Rn) and @X(Rn), cover every slot the
    // value-range analysis proves Rn can index: the whole table is data,
    // before the base as well as after (APB indexes its bit table at 9746
    // with negative offsets).
    if (!options.reads.empty()) {
        const auto reads = cfg::read_seeds(qilin::native_path(options.reads));
        if (!reads) {
            std::print(stderr, "qilin: {}\n", reads.error());
            return 1;
        }
        for (const auto& loc : *reads) {
            const auto p = loc.physical() / 2;
            if (p < words && cls[p] == unknown) cls[p] = data;
        }
    }
    const auto ranges = vrange::analyze(program);
    for (const auto& [loc, node] : program.nodes()) {
        if (!node.insn.legal) continue;
        const auto at = ranges.find(loc);
        if (at == ranges.end()) continue;
        for (const auto* op : {&node.insn.src, &node.insn.dst}) {
            if ((op->mode != t11::index && op->mode != t11::index_deferred) || op->reg > 5) continue;
            const auto range = at->second[op->reg];
            if (range.top() || range.size() > 4096) continue;
            const bool byte = node.insn.byte && op->mode == t11::index;
            for (std::int32_t r = range.lo; r <= range.hi; ++r) {
                const auto address = static_cast<std::uint16_t>(op->word + r);
                std::uint32_t physical = 0;
                if (address >= 0x8000) {
                    physical = address;
                } else if (address >= 0x4000 && loc.window() == (address >= 0x6000 ? 1 : 0)) {
                    physical = sys2::rom_offset(address, loc.bank, loc.bank);
                } else {
                    continue;
                }
                if (!byte && (physical & 1)) continue;
                if (physical / 2 < words && cls[physical / 2] == unknown) cls[physical / 2] = data;
            }
        }
    }
    std::vector<bool> run_from(words, false);
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& area : areas) {
            const std::uint32_t end = area.first + area.size;
            for (std::uint32_t p = area.first; p < end; ++p) {
                if (!referenced[p] || run_from[p] || (cls[p] != unknown && cls[p] != data)) continue;
                run_from[p] = true;
                // Through words already data (slots of indexed tables), up to code or a jump table.
                for (std::uint32_t q = p; q < end && (cls[q] == unknown || cls[q] == data); ++q) cls[q] = data;
                changed = true;
            }
        }
        for (std::uint32_t p = 0; p < words; ++p) {
            if (area_of[p] < 0 || (cls[p] != data && cls[p] != table && cls[p] != data_bank)) continue;
            const auto v = word_at(p * 2);
            if (!v || (*v & 1)) continue;
            const Area& area = areas[static_cast<std::size_t>(area_of[p])];
            std::uint32_t target = 0;
            if (*v >= 0x8000) target = *v;
            else if (area.code_bank && area.bank_first() && *v >= area.cpu_base && *v < area.cpu_base + 0x2000)
                target = area.first * 2 + (*v - area.cpu_base);
            else continue;
            if (target / 2 < words && cls[target / 2] == unknown && !referenced[target / 2]) {
                referenced[target / 2] = true;
                changed = true;
            }
        }
    }

    Totals all, data_banks;
    int status = 0;
    std::vector<std::string> listing;
    print_header();
    for (const auto& area : areas) {
        Totals t;
        const std::uint32_t end = area.first + area.size;
        // Fill: four or more words of 0x0000 or 0xFFFF.
        for (std::uint32_t p = area.first; p < end;) {
            const auto v = word_at(p * 2);
            if (cls[p] != unknown || !v || (*v != 0x0000 && *v != 0xffff)) {
                ++p;
                continue;
            }
            std::uint32_t q = p;
            while (q < end && cls[q] == unknown && word_at(q * 2) == v) ++q;
            if (q - p >= 4)
                for (std::uint32_t r = p; r < q; ++r) cls[r] = fill;
            p = q;
        }
        // A table's first entries, before the slot code refers to: unknown
        // words that carry on, backwards, the doubling or the constant step
        // of four or more data words after them (a bit table indexed from
        // its fourth entry, say).
        for (std::uint32_t p = area.first; p + 1 < end; ++p) {
            if (cls[p] != unknown || cls[p + 1] != data) continue;
            std::array<std::int32_t, 4> after{};
            bool ok = p + 5 <= end;
            for (std::uint32_t i = 0; ok && i < 4; ++i) {
                const auto v = word_at((p + 1 + i) * 2);
                ok = v && cls[p + 1 + i] == data;
                if (ok) after[i] = *v;
            }
            if (!ok) continue;
            const std::int32_t step = after[1] - after[0];
            const bool stepped = step != 0 && after[2] - after[1] == step && after[3] - after[2] == step;
            const bool doubled = after[0] != 0 && after[1] == 2 * after[0] && after[2] == 2 * after[1] && after[3] == 2 * after[2];
            if (!stepped && !doubled) continue;
            std::int32_t next = after[0];
            for (std::uint32_t q = p + 1; q-- > area.first && cls[q] == unknown;) {
                const std::int32_t want = doubled ? (next % 2 == 0 ? next / 2 : -1) : next - step;
                const auto v = word_at(q * 2);
                if (want <= 0 || !v || *v != want) break;
                cls[q] = data;
                next = want;
            }
        }
        // What's left, in stretches; a stretch that decodes like code from its start is a candidate.
        for (std::uint32_t p = area.first; p < end;) {
            if (cls[p] != unknown) {
                ++p;
                continue;
            }
            std::uint32_t q = p;
            while (q < end && cls[q] == unknown) ++q;
            const cfg::Loc at{static_cast<std::uint16_t>(area.cpu_base + (p - area.first) * 2), area.code_bank ? area.bank : std::uint8_t{0}};
            const bool code_like = area.code_bank && program.plausible_code(at, true);
            if (code_like) {
                ++t.candidates;
                t.candidate_bytes += 2 * (q - p);
                status = 3;
            }
            if (options.list && area.code_bank)
                listing.push_back(std::format("  {:<10} +{:<5} unknown{}", at.name(), 2 * (q - p), code_like ? ", decodes like code" : ""));
            p = q;
        }
        for (std::uint32_t p = area.first; p < end; ++p) t.bytes[cls[p]] += 2;
        for (int c = 0; c < 6; ++c) (area.code_bank ? all : data_banks).bytes[c] += t.bytes[c];
        all.candidates += t.candidates;
        all.candidate_bytes += t.candidate_bytes;
        if (area.code_bank) print_row(area.name, t);
    }
    print_row("data banks", data_banks);
    print_row("T-11 code areas", all);
    for (const auto& line : listing) std::print("{}\n", line);

    if (!options.check_seeds.empty()) {
        const auto seeds = cfg::read_seeds(qilin::native_path(options.check_seeds));
        if (!seeds) {
            std::print(stderr, "qilin: {}\n", seeds.error());
            return 1;
        }
        std::map<std::string, std::vector<std::string>> by_class;
        for (const auto& s : *seeds) {
            const auto p = s.physical() / 2;
            const bool start = program.nodes().contains(s);
            const std::string where = p < words ? class_name[cls[p]] : "outside ROM";
            by_class[start ? std::string("an instruction the analysis found") : "inside " + where].push_back(s.name());
        }
        std::print("\nSeeds in {}:\n", options.check_seeds);
        for (const auto& [where, list] : by_class) {
            std::string names;
            for (const auto& n : list) names += " " + n;
            std::print("  {} at {}:{}\n", list.size(), where, names);
        }
    }
    return status;
}

// ---- 6502 -------------------------------------------------------------------

int sound_cpu(const rom::RomSet& set, const Options& options) {
    const auto rom = set.region(rom::Region::sound_cpu);
    const auto program = m6502::Program::analyze(rom);
    std::vector<std::uint8_t> cls(0x10000, unknown);
    std::vector<bool> referenced(0x10000, false);
    for (const auto& [pc, node] : program.nodes()) {
        for (unsigned i = 0; i < node.insn.length; ++i) cls[(pc + i) & 0xffff] = code;
        using m6502::Mode;
        switch (node.insn.mode) {
        case Mode::aba: case Mode::abx: case Mode::aby: case Mode::ind:
            if (node.insn.operand >= m6502::Program::rom_start) referenced[node.insn.operand] = true;
            break;
        default: break;
        }
    }
    for (std::uint32_t v = 0xfffa; v < 0x10000; ++v) cls[v] = table;   // the vectors
    Totals t;
    std::vector<std::string> listing;
    const std::uint32_t first = m6502::Program::rom_start, end = 0x10000;
    for (std::uint32_t p = first; p < end; ++p) {
        if (!referenced[p] || cls[p] != unknown) continue;
        for (std::uint32_t q = p; q < end && cls[q] == unknown; ++q) cls[q] = data;
    }
    for (std::uint32_t p = first; p < end;) {
        const std::uint8_t v = p < rom.size() ? rom[p] : 0;
        if (cls[p] != unknown || (v != 0x00 && v != 0xff)) {
            ++p;
            continue;
        }
        std::uint32_t q = p;
        while (q < end && cls[q] == unknown && q < rom.size() && rom[q] == v) ++q;
        if (q - p >= 4)
            for (std::uint32_t r = p; r < q; ++r) cls[r] = fill;
        p = std::max(q, p + 1);
    }
    for (std::uint32_t p = first; p < end;) {
        if (cls[p] != unknown) {
            ++p;
            continue;
        }
        std::uint32_t q = p;
        while (q < end && cls[q] == unknown) ++q;
        if (options.list) listing.push_back(std::format("  {:04X}       +{:<5} unknown", p, q - p));
        p = q;
    }
    for (std::uint32_t p = first; p < end; ++p) t.bytes[cls[p]] += 1;
    print_row("6502 ROM", t);
    for (const auto& line : listing) std::print("{}\n", line);
    return 0;
}

} // namespace

int report(const rom::RomSet& set, const Options& options) {
    const auto program = cfg::Program::analyze(set, options.seeds);
    std::print("Every ROM byte, by what the analysis makes of it (bytes):\n\n");
    const int main_status = main_cpu(set, options, program);
    std::print("\n");
    sound_cpu(set, options);
    return main_status;
}

} // namespace qilin::coverage
