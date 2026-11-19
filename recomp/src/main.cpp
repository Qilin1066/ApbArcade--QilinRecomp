// qilin: the recompiler's command line.
//
//   qilin --version
//   qilin verify <rom-dir>                      check the 36 rev 7 files (size and SHA-1)
//   qilin insns  <rom-dir> [--cpu main|sound] [--banks B0,B1] [--from HEX] [--to HEX] [-o FILE]
//                                               linear listing at CPU addresses: T-11
//                                               (main, the default) or 6502 (sound)
//   qilin cfg    <rom-dir> [--cpu main|sound] [--seeds FILE] [--all]
//                                               control flow from reset: what is reached,
//                                               and everything not yet followed
//   qilin lift   <rom-dir> [--cpu main|sound] [--seeds FILE] [--service-interval CYCLES] --out DIR
//                                               lift every function to C++ (DIR is gitignored
//                                               when it's gen/: never commit generated code)
//
// Nothing read from the ROMs is written anywhere but the output you name.

#include "cfg.hpp"
#include "coverage.hpp"
#include "lifter.hpp"
#include "m6502_cfg.hpp"
#include "m6502_decoder.hpp"
#include "m6502_lifter.hpp"
#include "romset.hpp"
#include "system2.hpp"
#include "t11_decoder.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace rom = qilin::rom;
namespace t11 = qilin::t11;

int usage() {
    std::print(stderr,
               "usage: qilin --version\n"
               "       qilin verify <rom-dir>\n"
               "       qilin insns <rom-dir> [--cpu main|sound] [--banks B0,B1] [--from HEX] [--to HEX] [-o FILE]\n"
               "       qilin cfg <rom-dir> [--cpu main|sound] [--seeds FILE] [--all] [--why FILE]\n"
               "       qilin lift <rom-dir> [--cpu main|sound] [--seeds FILE] [--service-interval CYCLES] --out DIR\n"
               "       qilin coverage <rom-dir> [--seeds FILE] [--reads FILE] [--check-seeds FILE] [--list]\n");
    return 2;
}

std::optional<unsigned> parse_number(std::string_view text, int base) {
    if (base == 16 && (text.starts_with("0x") || text.starts_with("0X"))) {
        text.remove_prefix(2);
    }
    unsigned value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

int verify(const std::vector<std::string_view>& args) {
    if (args.size() != 1) {
        return usage();
    }
    unsigned matched = 0;
    const auto checks = rom::verify(args[0]);
    for (const auto& c : checks) {
        switch (c.status) {
        case rom::FileCheck::Status::ok:
            ++matched;
            break;
        case rom::FileCheck::Status::missing:
            std::print("{:<16} missing\n", c.file->name);
            break;
        case rom::FileCheck::Status::wrong_size:
            std::print("{:<16} {} bytes, expected {}\n", c.file->name, c.actual_size, c.file->size);
            break;
        case rom::FileCheck::Status::wrong_digest:
            std::print("{:<16} wrong SHA-1\n", c.file->name);
            break;
        }
    }
    std::print("APB rev 7: {} of {} files match by size and SHA-1.\n", matched, checks.size());
    return matched == checks.size() ? 0 : 1;
}

int insns(const std::vector<std::string_view>& args) {
    if (args.empty()) {
        return usage();
    }
    unsigned bank0 = 0, bank1 = 0, from = 0x8000, to = 0x10000;
    bool sound = false;
    bool from_given = false;
    std::string output;
    for (std::size_t i = 1; i < args.size(); i += 2) {
        const auto value = i + 1 < args.size() ? args[i + 1] : std::string_view{};
        if (args[i] == "--cpu" && (value == "main" || value == "sound")) {
            sound = value == "sound";
        } else if (args[i] == "--banks" && !value.empty()) {
            const auto comma = value.find(',');
            const auto b0 = parse_number(value.substr(0, comma), 10);
            const auto b1 = comma == std::string_view::npos ? std::nullopt : parse_number(value.substr(comma + 1), 10);
            if (!b0 || !b1 || *b0 >= qilin::sys2::rom_bank_count || *b1 >= qilin::sys2::rom_bank_count) {
                return usage();
            }
            bank0 = *b0;
            bank1 = *b1;
        } else if ((args[i] == "--from" || args[i] == "--to") && !value.empty()) {
            const auto address = parse_number(value, 16);
            if (!address || *address > 0x10000) {
                return usage();
            }
            (args[i] == "--from" ? from : to) = *address;
            from_given = from_given || args[i] == "--from";
        } else if (args[i] == "-o" && !value.empty()) {
            output = value;
        } else {
            return usage();
        }
    }
    if (sound && !from_given) {
        from = 0x4000;
    }
    if (!sound) {
        from &= ~1u;
        to &= ~1u;
    }
    if (from < 0x4000) {
        std::print(stderr, "qilin: the listing covers ROM (0x4000-0xFFFF)\n");
        return 2;
    }
    const auto set = rom::RomSet::load(args[0]);
    if (!set) {
        std::print(stderr, "qilin: {}\n", set.error());
        return 1;
    }

    std::ofstream file;
    if (!output.empty()) {
        file.open(output, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::print(stderr, "qilin: can't write {}\n", output);
            return 1;
        }
    }
    std::ostream& out = output.empty() ? std::cout : file;
    if (sound) {
        // The sound CPU's ROM is flat: CPU address = offset in the region.
        const auto bytes = set->region(rom::Region::sound_cpu);
        const auto byte = [&](unsigned address) {
            return address < 0x10000 ? bytes[address] : std::uint8_t{0};
        };
        out << std::format("; 6502 at CPU addresses {:04X}-{:04X} (MAME syntax)\n", from, to - 1);
        for (unsigned pc = from; pc < to;) {
            const auto insn = qilin::m6502::decode(byte(pc), byte(pc + 1), byte(pc + 2));
            std::string hex;
            for (unsigned i = 0; i < 3; ++i) {
                hex += i < insn.length ? std::format("{:02X} ", byte(pc + i)) : std::string(3, ' ');
            }
            out << std::format("{:04X}  {} {:>2}{}  {}{}\n", pc, hex, insn.cycles, insn.page_penalty ? "+" : " ",
                               qilin::m6502::disassemble(insn, static_cast<std::uint16_t>(pc)),
                               insn.documented ? "" : "    ; undocumented");
            pc += insn.length;
        }
        return 0;
    }
    const auto word = [&](unsigned address) -> std::optional<std::uint16_t> {
        if (address >= 0x10000) {
            return std::nullopt;
        }
        return set->word(rom::Region::main_cpu,
                         qilin::sys2::rom_offset(static_cast<std::uint16_t>(address), static_cast<std::uint8_t>(bank0),
                                                 static_cast<std::uint8_t>(bank1)));
    };
    out << std::format("; T-11 at CPU addresses {:04X}-{:04X}, banks {}/{} (MAME syntax, octal)\n", from, to - 2, bank0, bank1);
    for (unsigned pc = from; pc < to;) {
        const auto w0 = word(pc);
        if (!w0) {
            out << std::format("{:04X}  {:06o}  (no ROM here)\n", pc, pc);
            pc += 2;
            continue;
        }
        const auto insn = t11::decode(*w0, word(pc + 2).value_or(0), word(pc + 4).value_or(0));
        const unsigned length = insn.legal ? insn.words : 1u;
        std::string hex;
        for (unsigned i = 0; i < 3; ++i) {
            hex += i < length ? std::format("{:04X} ", word(pc + 2 * i).value_or(0)) : std::string(5, ' ');
        }
        out << std::format("{:04X}  {:06o}  {} {:>3}  {}{}\n", pc, pc, hex, insn.cycles, t11::disassemble(insn, static_cast<std::uint16_t>(pc)),
                           insn.legal ? "" : "    ; traps on the T-11");
        pc += 2 * length;
    }
    return 0;
}

int sound_cfg_report(const rom::RomSet& set, bool all, std::optional<unsigned> at) {
    namespace m6502 = qilin::m6502;
    const auto program = m6502::Program::analyze(set.region(rom::Region::sound_cpu));
    if (at) {
        // Where control reaches this address from.
        for (const auto& [pc, node] : program.nodes()) {
            const bool calls = node.transfer == m6502::Transfer::call && node.target == *at;
            if (calls || std::ranges::find(node.next, *at) != node.next.end()) {
                std::print("{:04X}  {}{}\n", pc, m6502::disassemble(node.insn, pc),
                           node.transfer == m6502::Transfer::table ? "    ; table" : "");
            }
        }
        return 0;
    }
    std::size_t bytes = 0;
    for (const auto& [pc, node] : program.nodes()) {
        bytes += node.insn.length;
    }
    std::print("Reached {} 6502 instructions ({} bytes of the ROM's 49152, {:.1f}%) from reset {:04X}, NMI {:04X} "
               "and IRQ {:04X}.\n",
               program.nodes().size(), bytes, 100.0 * bytes / 49152, program.reset_entry(), program.nmi_entry(),
               program.irq_entry());
    std::print("{} function entries, {} return sites.\n", program.functions().size(), program.return_sites().size());
    std::array<unsigned, 48> per_kib{};
    for (const auto& [pc, node] : program.nodes()) {
        per_kib[(pc - 0x4000u) >> 10] += node.insn.length;
    }
    std::print("Code bytes per KiB:");
    for (std::size_t i = 0; i < per_kib.size(); ++i) {
        std::print("{}{:04X}:{}", i % 12 == 0 ? "\n  " : " ", 0x4000 + 0x400 * i, per_kib[i]);
    }
    std::print("\n");
    std::map<m6502::Issue, std::vector<const m6502::Finding*>> grouped;
    for (const auto& f : program.findings()) {
        grouped[f.issue].push_back(&f);
    }
    for (const auto& [issue, list] : grouped) {
        std::print("\n{} ({}):\n", m6502::issue_name(issue), list.size());
        for (std::size_t i = 0; i < list.size() && (all || i < 12); ++i) {
            std::print("  {:04X}  {}\n", list[i]->at, list[i]->detail);
        }
        if (!all && list.size() > 12) {
            std::print("  ... {} more (--all)\n", list.size() - 12);
        }
    }
    return 0;
}

int cfg_report(const std::vector<std::string_view>& args) {
    bool all = false;
    bool sound = false;
    std::optional<unsigned> at;
    std::vector<qilin::cfg::Loc> seeds, why;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--all") {
            all = true;
        } else if (args[i] == "--why" && i + 1 < args.size()) {
            auto loaded = qilin::cfg::read_seeds(std::string(args[++i]));
            if (!loaded) {
                std::print(stderr, "qilin: {}\n", loaded.error());
                return 1;
            }
            why = std::move(*loaded);
        } else if (args[i] == "--seeds" && i + 1 < args.size()) {
            auto loaded = qilin::cfg::read_seeds(std::string(args[++i]));
            if (!loaded) {
                std::print(stderr, "qilin: {}\n", loaded.error());
                return 1;
            }
            seeds = std::move(*loaded);
        } else if (args[i] == "--from-where" && i + 1 < args.size() && parse_number(args[i + 1], 16)) {
            at = parse_number(args[++i], 16);
        } else if (args[i] == "--cpu" && i + 1 < args.size() && (args[i + 1] == "main" || args[i + 1] == "sound")) {
            sound = args[++i] == "sound";
        } else {
            return usage();
        }
    }
    if (args.empty()) {
        return usage();
    }
    const auto set = rom::RomSet::load(args[0]);
    if (!set) {
        std::print(stderr, "qilin: {}\n", set.error());
        return 1;
    }
    if (sound) {
        return sound_cfg_report(*set, all, at);
    }
    const auto program = qilin::cfg::Program::analyze(*set, seeds);
    if (!why.empty()) {
        // For each location, the nearest function entries control reaches it from, and how each was found.
        std::map<qilin::cfg::Loc, std::vector<qilin::cfg::Loc>> into;
        for (const auto& [loc, node] : program.nodes()) {
            for (const auto& n : node.next) into[n].push_back(loc);
            if (node.target) into[*node.target].push_back(loc);
        }
        const auto how = [&](const qilin::cfg::Loc& e) {
            std::string s;
            if (std::ranges::count(seeds, e)) s += " seed";
            if (program.prologue_entries().contains(e)) s += " prologue";
            if (program.bank_table_entries().contains(e)) s += " bank-table";
            if (program.pointer_entries().contains(e)) s += " pointer-immediate";
            if (program.data_pointer_entries().contains(e)) s += " data-pointer";
            if (program.gap_entries().contains(e)) s += " gap";
            return s.empty() ? std::string(" reached") : s;
        };
        for (const auto& w : why) {
            std::set<qilin::cfg::Loc> seen{w};
            std::vector<qilin::cfg::Loc> todo{w};
            std::string found;
            while (!todo.empty()) {
                const auto here = todo.back();
                todo.pop_back();
                if (program.functions().contains(here)) {
                    found += std::format(" {}({})", here.name(), how(here).substr(1));
                    if (how(here) != " reached") continue;
                }
                if (const auto it = into.find(here); it != into.end())
                    for (const auto& from : it->second)
                        if (seen.insert(from).second) todo.push_back(from);
            }
            std::print("{}:{}\n", w.name(), found.empty() ? std::string(" no entry") : found);
        }
        return 0;
    }
    std::size_t fixed = 0, banked = 0, bytes = 0;
    std::set<unsigned> banks;
    for (const auto& [loc, node] : program.nodes()) {
        (loc.pc >= 0x8000 ? fixed : banked) += 1;
        bytes += 2u * (node.insn.legal ? node.insn.words : 1u);
        if (loc.pc < 0x8000) {
            banks.insert(loc.bank);
        }
    }
    std::print("Reached {} T-11 instructions ({} bytes) from reset: {} in the fixed ROM, {} in {} banks.\n",
               program.nodes().size(), bytes, fixed, banked, banks.size());
    // Code bytes (opcodes and operand words) of the fixed ROM and of the code banks.
    std::set<std::uint32_t> code;
    for (const auto& [loc, node] : program.nodes()) {
        const unsigned words = node.insn.legal ? node.insn.words : 1u;
        for (unsigned i = 0; i < words; ++i) {
            code.insert(qilin::cfg::Loc{static_cast<std::uint16_t>(loc.pc + 2 * i), loc.bank}.physical());
        }
    }
    const auto fixed_bytes = 2 * static_cast<std::size_t>(std::ranges::count_if(code, [](auto p) { return p < 0x10000; }));
    const auto banked_bytes = 2 * code.size() - fixed_bytes;
    std::print("Code bytes: {} of the fixed ROM's 32768 ({:.1f}%), {} of the code banks' {} ({:.1f}%).\n", fixed_bytes,
               100.0 * fixed_bytes / 32768, banked_bytes, program.bank_windows().size() * 8192,
               100.0 * banked_bytes / std::max<std::size_t>(1, program.bank_windows().size() * 8192));
    std::print("{} function entries ({} from register-save prologues, {} from bank entry tables, {} from pointer "
               "immediates), {} return sites.\n",
               program.functions().size(), program.prologue_entries().size(), program.bank_table_entries().size(),
               program.pointer_entries().size(), program.return_sites().size());
    const auto list_entries = [](std::string_view what, const auto& entries) {
        if (entries.empty()) return;
        std::string names;
        for (const auto& loc : entries) names += " " + loc.name();
        std::print("{} {}:{}.\n", entries.size(), what, names);
    };
    if (all) {
        list_entries("entries from register-save prologues", program.prologue_entries());
        list_entries("entries from pointer immediates", program.pointer_entries());
    }
    list_entries("entries from code pointers in ROM data", program.data_pointer_entries());
    list_entries("entries in ROM nothing else reaches", program.gap_entries());
    std::size_t proven = 0, heuristic = 0;
    for (const auto& [loc, node] : program.nodes()) {
        if (node.flow == qilin::cfg::Flow::table) {
            (node.heuristic_table ? heuristic : proven) += 1;
        }
    }
    std::print("Jump tables: {} sized by value-range analysis, {} by reading entries (checked at run time).\n", proven,
               heuristic);
    std::string windows;
    for (const auto& [bank, window] : program.bank_windows()) {
        windows += std::format(" {}:{}", bank, window);
    }
    std::print("Code banks and the window each runs in:{}.\n", windows);
    for (const auto& [vector, handlers] : program.vector_stores()) {
        std::string list;
        for (const auto h : handlers) {
            list += std::format(" {:04X}", h);
        }
        std::print("Vector {:03o} is set to{}.\n", vector, list);
    }
    std::map<qilin::cfg::Issue, std::vector<const qilin::cfg::Finding*>> grouped;
    for (const auto& f : program.findings()) {
        grouped[f.issue].push_back(&f);
    }
    for (const auto& [issue, list] : grouped) {
        std::print("\n{} ({}):\n", qilin::cfg::describe(issue), list.size());
        for (std::size_t i = 0; i < list.size() && (all || i < 12); ++i) {
            std::print("  {:<9} {}\n", list[i]->at.name(), list[i]->detail);
        }
        if (!all && list.size() > 12) {
            std::print("  ... {} more (--all)\n", list.size() - 12);
        }
    }
    return 0;
}

int lift(const std::vector<std::string_view>& args) {
    std::string_view out_dir;
    bool sound = false;
    std::vector<qilin::cfg::Loc> seeds;
    qilin::lift::Options options;
    bool service_interval_given = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1 < args.size()) {
            out_dir = args[++i];
        } else if (args[i] == "--service-interval" && i + 1 < args.size()) {
            const auto v = args[++i];
            if (std::from_chars(v.data(), v.data() + v.size(), options.service_interval).ec != std::errc{}) return usage();
            service_interval_given = true;
        } else if (args[i] == "--seeds" && i + 1 < args.size()) {
            auto loaded = qilin::cfg::read_seeds(std::string(args[++i]));
            if (!loaded) {
                std::print(stderr, "qilin: {}\n", loaded.error());
                return 1;
            }
            seeds = std::move(*loaded);
        } else if (args[i] == "--cpu" && i + 1 < args.size() && (args[i + 1] == "main" || args[i + 1] == "sound")) {
            sound = args[++i] == "sound";
        } else {
            return usage();
        }
    }
    if (args.empty() || out_dir.empty()) {
        return usage();
    }
    const auto set = rom::RomSet::load(args[0]);
    if (!set) {
        std::print(stderr, "qilin: {}\n", set.error());
        return 1;
    }
    std::vector<std::pair<std::string, std::string>> files;
    std::vector<std::string> failures;
    std::size_t functions = 0, instructions = 0;
    if (sound) {
        const auto program = qilin::m6502::Program::analyze(set->region(rom::Region::sound_cpu));
        qilin::lift6502::Options sound_options;
        sound_options.service_interval = options.service_interval;
        if (!service_interval_given) sound_options = {};
        auto result = qilin::lift6502::lift(program, sound_options);
        for (auto& f : result.files) files.emplace_back(std::move(f.name), std::move(f.text));
        failures = std::move(result.failures);
        functions = result.functions;
        instructions = result.instructions;
    } else {
        const auto program = qilin::cfg::Program::analyze(*set, seeds);
        auto result = qilin::lift::lift(program, options);
        for (auto& f : result.files) files.emplace_back(std::move(f.name), std::move(f.text));
        failures = std::move(result.failures);
        functions = result.functions;
        instructions = result.instructions;
    }
    if (!failures.empty()) {
        for (const auto& f : failures) {
            std::print(stderr, "qilin: can't lift {}\n", f);
        }
        std::print(stderr, "qilin: {} instructions can't be lifted; nothing written\n", failures.size());
        return 3;
    }
    const std::filesystem::path out(out_dir);
    std::error_code error;
    std::filesystem::create_directories(out, error);
    // Files already as they'd be written are left alone, so a build doesn't
    // recompile them; files an earlier lift wrote that this one doesn't
    // (this lifter's prefix, .cpp or .hpp) are removed, as the build compiles
    // every .cpp in the folder.
    std::size_t bytes = 0, changed = 0, removed = 0;
    std::set<std::string> written;
    for (const auto& [name, text] : files) {
        written.insert(name);
        bytes += text.size();
        if (std::ifstream existing{out / name, std::ios::binary}) {
            const std::string old((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (old == text) continue;
        }
        std::ofstream stream(out / name, std::ios::binary | std::ios::trunc);
        stream << text;
        if (!stream) {
            std::print(stderr, "qilin: can't write {}\n", (out / name).string());
            return 1;
        }
        ++changed;
    }
    const std::string prefix = sound ? "sound" : "lifted";
    for (const auto& entry : std::filesystem::directory_iterator(out, error)) {
        const std::string name = entry.path().filename().string();
        const std::string ext = entry.path().extension().string();
        if (entry.is_regular_file() && name.starts_with(prefix) && (ext == ".cpp" || ext == ".hpp") && !written.contains(name)) {
            std::filesystem::remove(entry.path(), error);
            ++removed;
        }
    }
    std::print("Lifted {} {} functions ({} instructions with shared code counted per function) into {} files, {} KiB, in {}: "
               "{} written, {} unchanged{}.\n",
               functions, sound ? "6502" : "T-11", instructions, files.size(), bytes / 1024, out.string(), changed,
               files.size() - changed, removed ? std::format(", {} left over from an earlier lift removed", removed) : std::string());
    return 0;
}

} // namespace

int coverage_report(const std::vector<std::string_view>& args) {
    if (args.empty()) {
        return usage();
    }
    qilin::coverage::Options options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--list") {
            options.list = true;
        } else if (args[i] == "--seeds" && i + 1 < args.size()) {
            auto loaded = qilin::cfg::read_seeds(std::string(args[++i]));
            if (!loaded) {
                std::print(stderr, "qilin: {}\n", loaded.error());
                return 1;
            }
            options.seeds = std::move(*loaded);
        } else if (args[i] == "--check-seeds" && i + 1 < args.size()) {
            options.check_seeds = args[++i];
        } else if (args[i] == "--reads" && i + 1 < args.size()) {
            options.reads = args[++i];
        } else {
            return usage();
        }
    }
    const auto set = rom::RomSet::load(args[0]);
    if (!set) {
        std::print(stderr, "qilin: {}\n", set.error());
        return 1;
    }
    return qilin::coverage::report(*set, options);
}

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() == 1 && args[0] == "--version") {
        std::print("{}\n", QILIN_VERSION);
        return 0;
    }
    if (args.empty()) {
        return usage();
    }
    const auto command = args[0];
    args.erase(args.begin());
    if (command == "verify") {
        return verify(args);
    }
    if (command == "insns") {
        return insns(args);
    }
    if (command == "cfg") {
        return cfg_report(args);
    }
    if (command == "coverage") {
        return coverage_report(args);
    }
    if (command == "lift") {
        return lift(args);
    }
    return usage();
}
