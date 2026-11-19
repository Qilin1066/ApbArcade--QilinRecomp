#include "m6502_lifter.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <set>
#include <string_view>

namespace qilin::lift6502 {

namespace {

namespace m6502 = qilin::m6502;
using m6502::Transfer;

// MAME's om6502.lst, embedded by the build (see CMakeLists.txt).
constexpr unsigned char om6502_lst[] = {
#include "om6502_lst.inc"
};

// Opcode name ("lda_abx") -> its microcode lines, as written in om6502.lst.
const std::map<std::string, std::vector<std::string>>& microcode_table() {
    static const auto table = [] {
        std::map<std::string, std::vector<std::string>> t;
        const std::string_view text(reinterpret_cast<const char*>(om6502_lst), sizeof om6502_lst);
        std::vector<std::string>* body = nullptr;
        std::size_t pos = 0;
        while (pos < text.size()) {
            auto end = text.find('\n', pos);
            if (end == std::string_view::npos) end = text.size();
            std::string line(text.substr(pos, end - pos));
            pos = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) {
                body = nullptr;
            } else if (line[0] == '#') {
                continue;
            } else if (line[0] == '\t' || line[0] == ' ') {
                if (body) body->push_back(line);
            } else {
                body = &t[line];
            }
        }
        return t;
    }();
    return table;
}

// MAME's names in the microcode -> lifted code's.
const std::map<std::string_view, std::string_view>& identifiers() {
    static const std::map<std::string_view, std::string_view> map = {
        {"m_A", "a"}, {"m_X", "x"}, {"m_Y", "y"}, {"m_P", "p"}, {"m_SP", "sp"}, {"m_PC", "pc"},
        {"m_TMP", "tmp"}, {"m_TMP2", "tmp2"},
        {"read", "Q6_READ"}, {"write", "Q6_WRITE"}, {"read_pc", "Q6_PC"}, {"read_pc_noirq", "Q6_PC"},
        {"read_arg", "Q6_ARG"}, {"read_arg_noirq", "Q6_ARG"}, {"prefetch", "Q6_PREFETCH"},
        {"set_nz", "Q6_NZ"}, {"do_adc", "Q6_ADC"}, {"do_sbc", "Q6_SBC"}, {"do_cmp", "Q6_CMP"}, {"do_bit", "Q6_BIT"},
        {"do_asl", "Q6_ASL"}, {"do_lsr", "Q6_LSR"}, {"do_rol", "Q6_ROL"}, {"do_ror", "Q6_ROR"},
        {"dec_SP", "Q6_DEC_SP"}, {"inc_SP", "Q6_INC_SP"},
        {"set_h", "rt6502::set_h"}, {"set_l", "rt6502::set_l"}, {"page_changing", "rt6502::page_changing"},
        {"F_N", "rt6502::F_N"}, {"F_V", "rt6502::F_V"}, {"F_E", "rt6502::F_E"}, {"F_B", "rt6502::F_B"},
        {"F_D", "rt6502::F_D"}, {"F_I", "rt6502::F_I"}, {"F_Z", "rt6502::F_Z"}, {"F_C", "rt6502::F_C"},
        {"uint8_t", "std::uint8_t"}, {"int8_t", "std::int8_t"}, {"uint16_t", "std::uint16_t"}, {"int16_t", "std::int16_t"},
        {"if", "if"}, {"else", "else"}, {"true", "true"}, {"false", "false"},
    };
    return map;
}

std::optional<std::string> translate(const std::vector<std::string>& lines, std::string_view indent) {
    std::string out;
    for (const auto& raw : lines) {
        std::string_view line = raw;
        if (const auto comment = line.find("//"); comment != std::string_view::npos) line = line.substr(0, comment);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        std::size_t depth = 0;
        while (depth < line.size() && line[depth] == '\t') ++depth;
        line.remove_prefix(depth);
        if (line.empty()) continue;
        out += indent;
        out += std::string(4 * (depth > 0 ? depth - 1 : 0), ' ');
        for (std::size_t i = 0; i < line.size();) {
            const char ch = line[i];
            if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
                std::size_t j = i;
                while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) ++j;
                const auto word = line.substr(i, j - i);
                const auto it = identifiers().find(word);
                if (it == identifiers().end()) return std::nullopt;   // state lifted code doesn't have
                out += it->second;
                i = j;
            } else if (std::isdigit(static_cast<unsigned char>(ch))) {
                std::size_t j = i;
                while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) ++j;
                out += line.substr(i, j - i);
                i = j;
            } else {
                out += ch;
                ++i;
            }
        }
        out += '\n';
    }
    return out;
}

std::string hex(unsigned v) { return std::format("0x{:04X}", v); }
std::string label(std::uint16_t pc) { return std::format("L_{:04X}", pc); }

class FunctionWriter {
public:
    FunctionWriter(const m6502::Program& program, std::uint16_t entry, const std::set<std::uint16_t>& functions,
                   const Options& options, std::vector<std::string>& failures)
        : program_(program), entry_(entry), functions_(functions), options_(options), failures_(failures) {}

    // The function's text, and the entry points (index > 0) it offers dispatch.
    std::string write(std::vector<std::pair<std::uint16_t, unsigned>>& entries, std::size_t& instructions);

private:
    void line(const std::string& text) { body_ += "    " + indent_ + text + "\n"; }
    void instruction(std::uint16_t pc, const m6502::Node& node);
    void go(std::uint16_t target, bool service);
    void leave() { line("Q6_SYNC(); c.pc = pc; return;"); }

    const m6502::Program& program_;
    std::uint16_t entry_;
    const std::set<std::uint16_t>& functions_;
    const Options& options_;
    std::vector<std::string>& failures_;
    std::set<std::uint16_t> members_;
    std::set<std::uint16_t> referenced_;
    std::vector<std::uint16_t> resumes_;   // service point k resumes at resumes_[k]
    std::set<std::uint16_t> table_targets_;   // resumed at after a service in a table dispatch
    std::string body_;
    std::string indent_;
    unsigned straight_ = 0;                // cycles since the last service check, in emission order
    std::uint16_t pc_ = 0;
};

void FunctionWriter::go(std::uint16_t target, bool service) {
    if (!members_.contains(target)) {
        line(std::format("Q6_SYNC(); c.pc = {}; return;", hex(target)));
        return;
    }
    referenced_.insert(target);
    if (options_.service_interval != 0 && (service || target <= pc_ || straight_ >= options_.service_interval)) {
        const auto k = resumes_.size();
        resumes_.push_back(target);
        line(std::format("if (ic <= 0) [[unlikely]] {{ resume = {}; goto service; }}", k));
        straight_ = 0;
    }
    line(std::format("goto {};", label(target)));
}

void FunctionWriter::instruction(std::uint16_t pc, const m6502::Node& node) {
    const auto& insn = node.insn;
    pc_ = pc;
    body_ += std::format("    // {:04X}  {}\n", pc, m6502::disassemble(insn, pc));
    const auto code = insn.flow == m6502::Flow::halt ? std::nullopt : microcode(insn.opcode);
    if (!code || insn.flow == m6502::Flow::brk) {
        failures_.push_back(std::format("{:04X} {}", pc, m6502::disassemble(insn, pc)));
        line(std::format("Q6_SYNC(); c.pc = {}; b.unlifted(); return;", hex(pc)));
        return;
    }
    if (options_.service_interval == 0) {
        // Before every instruction, as MAME's 6502 runs whole instructions
        // until its slice is used up: the slice ends on the same instruction.
        const auto k = resumes_.size();
        resumes_.push_back(pc);
        line(std::format("if (ic <= 0) [[unlikely]] {{ resume = {}; goto service; }}", k));
        straight_ = 0;
    }
    // MAME decides on an interrupt at the end of each instruction, with the
    // I flag as it was before CLI, PLP or SEI changed it and after RTI did.
    const bool sets_i = insn.opcode == 0x58 || insn.opcode == 0x28 || insn.opcode == 0x78;
    if (sets_i) line("Q6_SYNC(); b.interrupt_window(-1); Q6_LOAD();");
    line(std::format("pc = {};", hex(static_cast<std::uint16_t>(pc + 1))));
    body_ += *code;
    if (insn.opcode == 0x58 || insn.opcode == 0x28) line("Q6_SYNC(); b.interrupt_window(1); Q6_LOAD();");
    if (insn.opcode == 0x40) line("Q6_SYNC(); b.interrupt_window(0); Q6_LOAD();");
    straight_ += insn.cycles;
    const auto next = insn.next(pc);

    switch (node.transfer) {
    case Transfer::next:
        go(next, false);
        break;
    case Transfer::branch:
        line(std::format("if (pc == {}) {{", hex(node.target)));
        indent_ = "    ";
        if (node.never_taken) {
            leave();   // the analysis proved no execution goes this way
        } else {
            go(node.target, false);
        }
        indent_.clear();
        line("}");
        if (node.always_taken) {
            leave();
        } else {
            go(next, false);
        }
        break;
    case Transfer::jump:
        line(std::format("if (pc == {}) {{", hex(node.target)));
        indent_ = "    ";
        go(node.target, false);
        indent_.clear();
        line("}");
        leave();
        break;
    case Transfer::call: {
        line("Q6_SYNC(); c.pc = pc;");
        if (functions_.contains(node.target)) {
            line(std::format("if (pc == {}) {}(b, 0); else if (!rt6502::dispatch(b)) b.unlifted();", hex(node.target),
                             function_name(node.target)));
        } else {
            line("if (!rt6502::dispatch(b)) b.unlifted();");
        }
        line(std::format("if (b.yielding || c.pc != {}) return;", hex(next)));
        line("Q6_LOAD();");
        go(next, false);
        break;
    }
    case Transfer::table:
        // A loop can run through a table, so the board gets its turn here
        // too. The targets are dispatch entries, for resuming after a yield.
        line("if (ic <= 0) [[unlikely]] {");
        line("    Q6_SYNC(); c.pc = pc; b.service();");
        line("    if (b.yielding || c.pc != pc) return;");
        line("    Q6_LOAD();");
        line("}");
        for (const auto target : node.table) {
            if (members_.contains(target)) table_targets_.insert(target);
        }
        straight_ = 0;
        if (std::ranges::any_of(node.table, [&](std::uint16_t t) { return members_.contains(t); })) {
            line("switch (pc) {");
            for (const auto target : node.table) {
                if (members_.contains(target)) {
                    referenced_.insert(target);
                    line(std::format("case {}: goto {};", hex(target), label(target)));
                }
            }
            line("default: break;");
            line("}");
        }
        leave();
        break;
    case Transfer::ret:
    case Transfer::rti:
    case Transfer::computed_jump:
        leave();
        break;
    case Transfer::brk:
    case Transfer::halt:
        break;   // handled above
    }
}

std::string FunctionWriter::write(std::vector<std::pair<std::uint16_t, unsigned>>& entries, std::size_t& instructions) {
    const auto& nodes = program_.nodes();
    std::vector<std::uint16_t> work{entry_};
    while (!work.empty()) {
        const auto at = work.back();
        work.pop_back();
        if (!nodes.contains(at) || !members_.insert(at).second) continue;
        for (const auto n : nodes.at(at).next) work.push_back(n);
    }
    instructions += members_.size();

    std::string code;
    for (const auto member : members_) {
        body_.clear();
        instruction(member, nodes.at(member));
        code += "@@" + label(member) + "@@\n" + body_;
    }
    // Entries dispatch may use: the start, each return site, and each service resume point.
    std::set<std::uint16_t> resume_sites;
    for (const auto member : members_) {
        if (member != entry_ && program_.return_sites().contains(member)) resume_sites.insert(member);
    }
    for (const auto target : resumes_) {
        if (target != entry_) resume_sites.insert(target);
    }
    for (const auto target : table_targets_) {
        if (target != entry_) resume_sites.insert(target);
    }
    referenced_.insert(entry_);
    referenced_.insert(resume_sites.begin(), resume_sites.end());

    std::string text;
    for (std::size_t pos = 0; pos < code.size();) {
        const auto start = code.find("@@", pos);
        if (start == std::string::npos) {
            text += code.substr(pos);
            break;
        }
        text += code.substr(pos, start - pos);
        const auto end = code.find("@@\n", start + 2);
        const auto name = code.substr(start + 2, end - start - 2);
        bool used = false;
        for (const auto r : referenced_) used = used || label(r) == name;
        if (used) text += name + ":\n";
        pos = end + 3;
    }

    const auto uses = [&](std::string_view name) {
        for (auto pos = text.find(name); pos != std::string::npos; pos = text.find(name, pos + 1)) {
            const auto ident = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; };
            const bool before = pos > 0 && ident(text[pos - 1]);
            const bool after = pos + name.size() < text.size() && ident(text[pos + name.size()]);
            if (!before && !after) return true;
        }
        return false;
    };
    std::string f = std::format("void {}(Board& b, unsigned entry) {{\n", function_name(entry_));
    f += "    rt6502::Cpu& c = b.cpu;\n"
         "    std::uint8_t a = 0, x = 0, y = 0, p = 0;\n"
         "    std::uint16_t sp = 0, pc = 0;\n"
         "    std::int32_t ic = 0;\n";
    if (uses("tmp")) f += "    std::uint16_t tmp = 0;\n";
    if (uses("tmp2")) f += "    std::uint8_t tmp2 = 0;\n";
    if (!resumes_.empty()) f += "    unsigned resume = 0;\n";
    f += "    Q6_LOAD();\n";
    if (resume_sites.empty()) {
        f += std::format("    static_cast<void>(entry);\n    goto {};\n", label(entry_));
    } else {
        f += "    switch (entry) {\n";
        unsigned k = 0;
        for (const auto site : resume_sites) {
            ++k;
            f += std::format("    case {}: goto {};\n", k, label(site));
            entries.emplace_back(site, k);
        }
        f += std::format("    default: goto {};\n    }}\n", label(entry_));
    }
    f += text;
    if (!resumes_.empty()) {
        // Board service: write back, let the board run (it may take an
        // interrupt, or end the slice), and resume only if control came back
        // to the same place.
        f += "service:\n    Q6_SYNC();\n    switch (resume) {\n";
        for (std::size_t k = 0; k < resumes_.size(); ++k) {
            f += std::format("    case {}: c.pc = {}; b.service(); if (b.yielding || c.pc != {}) return; Q6_LOAD(); goto {};\n",
                             k, hex(resumes_[k]), hex(resumes_[k]), label(resumes_[k]));
        }
        f += "    default: return;\n    }\n";
    }
    f += "}\n\n";
    return f;
}

} // namespace

std::string function_name(std::uint16_t entry) { return std::format("snd_{:04X}", entry); }

std::optional<std::string> microcode(std::uint8_t opcode) {
    const auto insn = m6502::decode(opcode);
    const auto name = std::format("{}_{}", m6502::mnemonic(insn.op), m6502::mode_name(insn.mode));
    const auto& table = microcode_table();
    const auto it = table.find(name);
    if (it == table.end()) return std::nullopt;
    return translate(it->second, "    ");
}

Result lift(const m6502::Program& program, const Options& options) {
    Result result;
    std::set<std::uint16_t> functions;
    for (const auto f : program.functions()) {
        if (program.nodes().contains(f)) functions.insert(f);
    }
    result.functions = functions.size();

    static constexpr const char* prelude =
        "// Generated by qilin lift from the APB rev 7 sound ROMs. Not for the repository.\n"
        "// Instruction bodies are MAME's 6502 microcode (om6502.lst, BSD-3-Clause, Olivier Galibert).\n"
        "#include \"qilin_sound_board.hpp\"\n"
        "#include \"sound_lifted.hpp\"\n\n"
        "#if defined(__GNUC__)\n"
        "#pragma GCC diagnostic ignored \"-Wunused-label\"\n"
        "#endif\n"
        "#if defined(_MSC_VER)\n"
        "#pragma warning(disable : 4244)   // MAME's microcode truncates to 8 bits implicitly, as the chip does\n"
        "#endif\n\n"
        "#define Q6_LOAD() (a = c.a, x = c.x, y = c.y, p = c.p, sp = c.sp, ic = b.icount)\n"
        "#define Q6_SYNC() (c.a = a, c.x = x, c.y = y, c.p = p, c.sp = sp, b.icount = ic)\n"
        "// Each bus cycle: the board is told the cycles left before it (MAME's\n"
        "// microcode accesses, then counts), so the chips see MAME's local time.\n"
        "#define Q6_READ(adr) (b.icount = ic--, b.read(static_cast<std::uint16_t>(adr)))\n"
        "#define Q6_WRITE(adr, val) (b.icount = ic--, b.write(static_cast<std::uint16_t>(adr), static_cast<std::uint8_t>(val)))\n"
        "#define Q6_PC() (b.icount = ic--, b.read_arg(pc))\n"
        "#define Q6_ARG(adr) (b.icount = ic--, b.read_arg(static_cast<std::uint16_t>(adr)))\n"
        "#define Q6_PREFETCH() (b.icount = ic--, b.read_sync(pc))\n"
        "#define Q6_NZ(v) rt6502::set_nz(p, static_cast<std::uint8_t>(v))\n"
        "#define Q6_ADC(v) rt6502::adc(a, p, static_cast<std::uint8_t>(v))\n"
        "#define Q6_SBC(v) rt6502::sbc(a, p, static_cast<std::uint8_t>(v))\n"
        "#define Q6_CMP(r, v) rt6502::cmp(p, static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(v))\n"
        "#define Q6_BIT(v) rt6502::bit(a, p, static_cast<std::uint8_t>(v))\n"
        "#define Q6_ASL(v) rt6502::asl(p, static_cast<std::uint8_t>(v))\n"
        "#define Q6_LSR(v) rt6502::lsr(p, static_cast<std::uint8_t>(v))\n"
        "#define Q6_ROL(v) rt6502::rol(p, static_cast<std::uint8_t>(v))\n"
        "#define Q6_ROR(v) rt6502::ror(p, static_cast<std::uint8_t>(v))\n"
        "#define Q6_DEC_SP() (sp = rt6502::set_l(sp, static_cast<std::uint8_t>(sp - 1)))\n"
        "#define Q6_INC_SP() (sp = rt6502::set_l(sp, static_cast<std::uint8_t>(sp + 1)))\n\n"
        "namespace qilin::gen6502 {\n\nusing rt6502::Board;\n\n";

    std::vector<std::pair<std::uint16_t, std::pair<std::string, unsigned>>> entries;
    std::vector<std::string> bodies;
    for (const auto f : functions) {
        std::vector<std::pair<std::uint16_t, unsigned>> resumes;
        FunctionWriter writer(program, f, functions, options, result.failures);
        bodies.push_back(writer.write(resumes, result.instructions));
        entries.push_back({f, {function_name(f), 0}});
        for (const auto& [where, index] : resumes) entries.push_back({where, {function_name(f), index}});
    }
    // One entry per address: a function start wins over a resume point in another function.
    std::ranges::stable_sort(entries, [](const auto& x, const auto& y) {
        return x.first < y.first || (x.first == y.first && x.second.second < y.second.second);
    });
    const auto [first, last] = std::ranges::unique(entries, [](const auto& x, const auto& y) { return x.first == y.first; });
    entries.erase(first, last);
    std::ranges::sort(result.failures);
    result.failures.erase(std::unique(result.failures.begin(), result.failures.end()), result.failures.end());

    std::string header = "// Generated by qilin lift. Not for the repository.\n#pragma once\n#include \"m6502_rt.hpp\"\n\n"
                         "namespace qilin::gen6502 {\n\nusing rt6502::Board;\n\n";
    for (const auto f : functions) header += std::format("void {}(Board& b, unsigned entry);\n", function_name(f));
    header += "\n} // namespace qilin::gen6502\n";
    result.files.push_back({"sound_lifted.hpp", header});

    for (std::size_t k = 0; k * options.functions_per_file < bodies.size(); ++k) {
        std::string text = prelude;
        for (std::size_t j = k * options.functions_per_file; j < std::min(bodies.size(), (k + 1) * options.functions_per_file); ++j) {
            text += bodies[j];
        }
        text += "} // namespace qilin::gen6502\n";
        result.files.push_back({std::format("sound_{}.cpp", k), text});
    }

    std::string dispatch =
        "// Generated by qilin lift. Not for the repository.\n"
        "#include \"qilin_sound_board.hpp\"\n#include \"sound_lifted.hpp\"\n\n#include <algorithm>\n\n"
        "namespace qilin::rt6502 {\n\nconst Entry lifted_entries[] = {\n";
    for (const auto& [where, fn] : entries) {
        dispatch += std::format("    {{{}, &gen6502::{}, {}}},\n", hex(where), fn.first, fn.second);
    }
    dispatch += std::format("}};\nconst unsigned lifted_entry_count = {};\n\n", entries.size());
    dispatch +=
        "// Every address of the ROM to its entry, one lookup: the sound CPU yields\n"
        "// at the end of each slice (every few instructions) and resumes through here.\n"
        "namespace {\n"
        "struct Table {\n"
        "    std::uint16_t at[0xc000] = {};   // 0x4000-0xFFFF: 1 + the entry's index, or 0\n"
        "    Table() {\n"
        "        for (unsigned i = 0; i < lifted_entry_count; ++i)\n"
        "            if (lifted_entries[i].pc >= 0x4000) at[lifted_entries[i].pc - 0x4000] = static_cast<std::uint16_t>(i + 1);\n"
        "    }\n"
        "};\n"
        "const Table table;\n"
        "} // namespace\n\n"
        "bool dispatch(Board& b) {\n"
        "    const std::uint16_t pc = b.cpu.pc;\n"
        "    if (pc < 0x4000) return false;\n"
        "    const unsigned i = table.at[pc - 0x4000];\n"
        "    if (i == 0) return false;\n"
        "    const Entry& e = lifted_entries[i - 1];\n"
        "    e.fn(b, e.index);\n"
        "    return true;\n"
        "}\n\n} // namespace qilin::rt6502\n";
    result.files.push_back({"sound_dispatch.cpp", dispatch});
    return result;
}

} // namespace qilin::lift6502
