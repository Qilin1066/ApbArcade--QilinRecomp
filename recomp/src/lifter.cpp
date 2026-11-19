#include "lifter.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <format>
#include <map>
#include <set>

namespace qilin::lift {
namespace {

using cfg::Flow;
using cfg::Loc;
using t11::Op;
using t11::Operand;

std::string label(Loc loc) {
    return loc.pc >= 0x8000 ? std::format("L_{:04X}", loc.pc) : std::format("L_{:04X}_b{}", loc.pc, loc.bank);
}

std::string hex(unsigned value) {
    return std::format("0x{:04X}", value & 0xffff);
}

// The C++ test for a branch condition, from MAME's handlers.
std::string condition(Op op) {
    switch (op) {
    case Op::bne: return "!z";
    case Op::beq: return "z";
    case Op::bge: return "n == v";
    case Op::blt: return "n != v";
    case Op::bgt: return "!z && n == v";
    case Op::ble: return "z || n != v";
    case Op::bpl: return "!n";
    case Op::bmi: return "n";
    case Op::bhi: return "!cf && !z";
    case Op::blos: return "cf || z";
    case Op::bvc: return "!v";
    case Op::bvs: return "v";
    case Op::bcc: return "!cf";
    case Op::bcs: return "cf";
    default: return "true";
    }
}

bool is_conditional(Op op) {
    return condition(op) != "true";
}

// Generates one function.
class FunctionWriter {
public:
    FunctionWriter(const cfg::Program& program, Loc entry, const std::set<Loc>& lifted_functions,
                   const Options& options, std::vector<std::string>& failures)
        : program_(program), entry_(entry), functions_(lifted_functions), options_(options), failures_(failures) {}

    // Returns the function's text, and the entry points (index > 0) it offers dispatch.
    std::string write(std::vector<std::pair<Loc, unsigned>>& entries, std::size_t& instructions);

private:
    // Per-instruction state.
    std::string& out() { return body_; }
    void line(const std::string& text) { body_ += "    " + text + "\n"; }

    std::string reg(unsigned r) const { return r == 7 ? hex(pc_) : std::format("r{}", r); }
    void fetch() {
        line(std::format("b.fetch({});", hex(pc_)));
        pc_ = static_cast<std::uint16_t>(pc_ + 2);
    }
    bool fail(const std::string& why) {
        failures_.push_back(std::format("{} {}: {}", at_.name(), t11::disassemble(*insn_, at_.pc), why));
        ok_ = false;
        return false;
    }
    std::string read(const std::string& address, bool byte) {
        return byte ? std::format("b.read8({})", address) : std::format("b.read16(static_cast<std::uint16_t>(({}) & 0xFFFE))", address);
    }
    void write(const std::string& address, const std::string& value, bool byte) {
        if (byte) {
            line(std::format("b.write8({}, static_cast<std::uint8_t>({}));", address, value));
        } else {
            line(std::format("b.write16(static_cast<std::uint16_t>(({}) & 0xFFFE), static_cast<std::uint16_t>({}));", address, value));
        }
        if (!address_is_constant_) {
            dynamic_write_ = true;
        }
    }

    // MAME's MAKE_EAB_* / MAKE_EAW_* into 'ea'. 'immediate_is_read': the
    // destination form of (PC)+, which MAME reads through the data bus.
    bool effective_address(const Operand& op, bool byte, const std::string& ea);
    // GET_S* and GET_D*: the operand's value into 'var' (int, as MAME's).
    bool get_source(const Operand& op, bool byte, const std::string& var);
    bool get_dest(const Operand& op, bool byte);           // into 'dst', address in 'ea'
    void put_dest_ea(const Operand& op, bool byte, const std::string& value);   // PUT_D*_EA / _DREG
    bool put_dest(const Operand& op, bool byte, const std::string& value, bool dummy_read);   // PUT_D*_xx

    void flags_nz(bool byte, const std::string& result) {
        line(byte ? std::format("n = (({}) >> 7) & 1; z = (({}) & 0xFF) == 0;", result, result)
                  : std::format("n = (({}) >> 15) & 1; z = (({}) & 0xFFFF) == 0;", result, result));
    }
    void flags_nzvc(bool byte, const std::string& src, const std::string& dst, const std::string& res) {
        flags_nz(byte, res);
        const int shift = byte ? 7 : 15;
        line(std::format("v = ((({0}) ^ ({1}) ^ ({2}) ^ (({2}) >> 1)) >> {3}) & 1; cf = (({2}) >> {4}) & 1;", src, dst, res,
                         shift, shift + 1));
    }

    bool instruction(const cfg::Node& node);
    void transfer(Loc target);                 // goto within the function, or leave
    void leave(const std::string& pc);          // write state back and return, PC = pc
    void call(const std::string& target, std::optional<Loc> static_target, std::uint16_t return_pc);
    void service_point(std::uint16_t resume_pc, bool when_due = false);
    void table_switch(const cfg::Node& node, const std::string& pc);   // goto a table's targets, else fall through

    const cfg::Program& program_;
    Loc entry_;
    const std::set<Loc>& functions_;
    const Options& options_;
    std::vector<std::string>& failures_;

    std::set<Loc> members_;
    std::string body_;
    std::set<Loc> referenced_;      // labels some goto uses
    std::vector<std::pair<std::uint16_t, Loc>> resumes_;   // service resume points
    Loc at_{};
    const t11::Instruction* insn_ = nullptr;
    std::uint16_t pc_ = 0;
    bool ok_ = true;
    bool address_is_constant_ = false;
    bool dynamic_write_ = false;
    unsigned straight_cycles_ = 0;
};

bool FunctionWriter::effective_address(const Operand& op, bool byte, const std::string& ea) {
    const unsigned r = op.reg;
    const auto name = std::format("r{}", r);
    const unsigned step = (byte && r < 6) ? 1 : 2;
    address_is_constant_ = false;
    switch (op.mode) {
    case t11::reg_deferred:
        line(std::format("{} = {};", ea, reg(r)));
        address_is_constant_ = r == 7;
        return true;
    case t11::autoinc:
        if (r == 7) {   // destination (PC)+: the word after, read as data
            line(std::format("{} = {};", ea, hex(pc_)));
            pc_ = static_cast<std::uint16_t>(pc_ + 2);
            address_is_constant_ = true;
            return true;
        }
        line(std::format("{0} = {1}; {1} = static_cast<std::uint16_t>({1} + {2});", ea, name, step));
        return true;
    case t11::autoinc_deferred:
        if (r == 7) {   // @#a
            const auto address = op.word;
            fetch();
            line(std::format("{} = {};", ea, hex(address)));
            address_is_constant_ = true;
            return true;
        }
        line(std::format("{0} = {1}; {1} = static_cast<std::uint16_t>({1} + 2); {0} = {2};", ea, name, read(ea, false)));
        return true;
    case t11::autodec:
        if (r == 7) return fail("-(PC) is not supported");
        line(std::format("{1} = static_cast<std::uint16_t>({1} - {2}); {0} = {1};", ea, name, step));
        return true;
    case t11::autodec_deferred:
        if (r == 7) return fail("@-(PC) is not supported");
        line(std::format("{1} = static_cast<std::uint16_t>({1} - 2); {0} = {2};", ea, name, read(name, false)));
        return true;
    case t11::index: {
        const auto x = op.word;
        fetch();
        if (r == 7) {
            line(std::format("{} = {};", ea, hex(x + pc_)));
            address_is_constant_ = true;
        } else {
            line(std::format("{} = static_cast<std::uint16_t>({} + {});", ea, hex(x), name));
        }
        return true;
    }
    case t11::index_deferred: {
        const auto x = op.word;
        fetch();
        const auto pointer = r == 7 ? hex(x + pc_) : std::format("static_cast<std::uint16_t>({} + {})", hex(x), name);
        line(std::format("{} = {};", ea, read(pointer, false)));
        return true;
    }
    default:
        return fail("register mode has no address");
    }
}

bool FunctionWriter::get_source(const Operand& op, bool byte, const std::string& var) {
    if (op.mode == t11::reg) {
        line(std::format("{} = {};", var, byte ? std::format("({} & 0xFF)", reg(op.reg)) : reg(op.reg)));
        return true;
    }
    if (op.immediate()) {   // ROPCODE(): the whole word, even for byte instructions
        const auto value = op.word;
        fetch();
        line(std::format("{} = {};", var, hex(value)));
        return true;
    }
    if (!effective_address(op, byte, "ea")) return false;
    line(std::format("{} = {};", var, read("ea", byte)));
    return true;
}

bool FunctionWriter::get_dest(const Operand& op, bool byte) {
    if (op.mode == t11::reg) {
        line(std::format("dst = {};", byte ? std::format("({} & 0xFF)", reg(op.reg)) : reg(op.reg)));
        return true;
    }
    if (!effective_address(op, byte, "ea")) return false;
    line(std::format("dst = {};", read("ea", byte)));
    return true;
}

void FunctionWriter::put_dest_ea(const Operand& op, bool byte, const std::string& value) {
    if (op.mode == t11::reg) {
        const auto name = std::format("r{}", op.reg);
        line(byte ? std::format("{0} = static_cast<std::uint16_t>(({0} & 0xFF00) | (({1}) & 0xFF));", name, value)
                  : std::format("{} = static_cast<std::uint16_t>({});", name, value));
        return;
    }
    write("ea", value, byte);
}

bool FunctionWriter::put_dest(const Operand& op, bool byte, const std::string& value, bool dummy_read) {
    if (op.mode == t11::reg) {
        put_dest_ea(op, byte, value);
        return true;
    }
    if (value != "res") line(std::format("res = {};", value));   // computed before the destination's bus cycles
    if (!effective_address(op, byte, "ea")) return false;
    if (dummy_read) {
        line(std::format("static_cast<void>({});", read("ea", byte)));
    }
    write("ea", "res", byte);
    return true;
}

void FunctionWriter::transfer(Loc target) {
    if (members_.contains(target)) {
        referenced_.insert(target);
        line(std::format("goto {};", label(target)));
    } else {
        leave(hex(target.pc));
    }
}

void FunctionWriter::leave(const std::string& pc) {
    line(std::format("QL_SYNC(); c.r[7] = {}; return;", pc));
}

// when_due: also when an interrupt would be taken now (after MTPS, where
// MAME checks the lines at once, not at the end of the timeslice).
void FunctionWriter::service_point(std::uint16_t resume_pc, bool when_due) {
    const auto index = resumes_.size();
    resumes_.emplace_back(resume_pc, at_);
    line(std::format("if (ic <= 0{}) [[unlikely]] {{ resume = {}; goto service; }}", when_due ? " || b.interrupt_due()" : "", index));
    body_ += std::format("S{}:\n", index);
    straight_cycles_ = 0;
}

// A jump through a table: a loop can run through it, so the board gets its
// turn here too (the resume address is the computed one), then the targets
// in this function are gotos; anything else falls through to leave().
void FunctionWriter::table_switch(const cfg::Node& node, const std::string& pc) {
    line("if (ic <= 0) [[unlikely]] {");
    line(std::format("    QL_SYNC(); c.r[7] = {}; b.service();", pc));
    line(std::format("    if (c.r[7] != {}) return;", pc));
    line("    QL_LOAD();");
    line("}");
    straight_cycles_ = 0;
    std::set<std::uint16_t> done;
    std::string cases;
    for (const auto& target : node.next) {
        if (done.insert(target.pc).second && members_.contains(target)) {
            referenced_.insert(target);
            cases += std::format("    case {}: goto {};\n", hex(target.pc), label(target));
        }
    }
    if (!cases.empty()) {
        line(std::format("switch ({}) {{", pc));
        out() += cases;   // indented like the case lines above
        line("default: break;");
        line("}");
    }
}

// A call: write back, call, and continue only if the callee came back here
// (and, for code in a window, with the window's bank unchanged).
void FunctionWriter::call(const std::string& target, std::optional<Loc> static_target, std::uint16_t return_pc) {
    line(std::format("QL_SYNC(); c.r[7] = {};", target));
    if (static_target && functions_.contains(*static_target)) {
        line(std::format("{}(b, 0);", function_name(*static_target)));
    } else {
        line("if (!rt::dispatch(b)) b.unlifted();");
    }
    const int w = at_.window();
    if (w >= 0) {
        line(std::format("if (c.r[7] != {} || b.bank[{}] != {}) return;", hex(return_pc), w, at_.bank));
    } else {
        line(std::format("if (c.r[7] != {}) return;", hex(return_pc)));
    }
    line("QL_LOAD();");
    straight_cycles_ = 0;
}

bool FunctionWriter::instruction(const cfg::Node& node) {
    const auto& i = node.insn;
    insn_ = &i;
    pc_ = at_.pc;
    dynamic_write_ = false;
    ok_ = true;
    out() += std::format("    // {:04X}  {}\n", at_.pc, t11::disassemble(i, at_.pc));
    line(std::format("ic -= {};", i.cycles));
    fetch();   // the opcode
    straight_cycles_ += i.cycles;

    const auto next_pc = static_cast<std::uint16_t>(at_.pc + 2 * (i.legal ? i.words : 1));
    // The location control falls through to, as the analysis placed it.
    std::optional<Loc> fall;
    for (const auto& n : node.next) {
        if (n.pc == next_pc) fall = n;
    }
    const auto go_next = [&] {
        if (dynamic_write_ && at_.window() >= 0) {
            // A write through a computed address may have switched the bank this code runs from.
            line(std::format("if (b.bank[{}] != {}) {{ QL_SYNC(); c.r[7] = {}; return; }}", at_.window(), at_.bank, hex(next_pc)));
        }
        if (fall) {
            if (straight_cycles_ >= options_.service_interval) service_point(next_pc);
            transfer(*fall);
        } else {
            leave(hex(next_pc));   // into a window whose bank isn't known here
        }
    };
    const bool B = i.byte;

    if (!i.legal || i.op == Op::bpt || i.op == Op::iot || i.op == Op::emt || i.op == Op::trap || i.op == Op::halt) {
        // trap_to / HALT: push PSW and PC, then the vector's PC and PSW.
        const int vector = i.op == Op::halt ? -1 : t11::trap_vector(i);
        line("QL_SYNC();");
        line("r6 = static_cast<std::uint16_t>(r6 - 2); b.write16(static_cast<std::uint16_t>(r6 & 0xFFFE), c.psw);");
        line(std::format("r6 = static_cast<std::uint16_t>(r6 - 2); b.write16(static_cast<std::uint16_t>(r6 & 0xFFFE), {});", hex(next_pc)));
        line("c.r[6] = r6;");
        if (vector < 0) {
            line("c.r[7] = 0x8004; c.psw = 0xE0;");   // HALT: the start address + 4, priority 7
        } else {
            line(std::format("c.r[7] = b.read16({}); c.psw = static_cast<std::uint8_t>(b.read16({}));", hex(vector), hex(vector + 2)));
        }
        line("b.icount = ic; b.service();");   // MAME checks interrupts after a trap
        line("if (!rt::dispatch(b)) b.unlifted();");
        const int w = at_.window();
        line(w >= 0 ? std::format("if (c.r[7] != {} || b.bank[{}] != {}) return;", hex(next_pc), w, at_.bank)
                    : std::format("if (c.r[7] != {}) return;", hex(next_pc)));
        line("QL_LOAD();");
        go_next();
        return ok_;
    }

    switch (i.op) {
    case Op::wait:
        line(std::format("QL_SYNC(); c.r[7] = {}; b.wait();", hex(next_pc)));
        line(std::format("if (c.r[7] != {}) return;", hex(next_pc)));
        line("QL_LOAD();");
        go_next();
        return ok_;
    case Op::reset:
        line("b.reset_output();");
        go_next();
        return ok_;
    case Op::mfpt:
        line("r0 = static_cast<std::uint16_t>((r0 & 0xFF00) | 4);");
        go_next();
        return ok_;
    case Op::rti:
    case Op::rtt:
        line("t = b.read16(static_cast<std::uint16_t>(r6 & 0xFFFE)); r6 = static_cast<std::uint16_t>(r6 + 2);");
        line("res = b.read16(static_cast<std::uint16_t>(r6 & 0xFFFE)); r6 = static_cast<std::uint16_t>(r6 + 2);");
        // MAME checks the interrupt lines at once after RTI and RTT.
        line("QL_SYNC(); c.r[7] = t; c.psw = static_cast<std::uint8_t>(res); if (b.interrupt_due()) [[unlikely]] b.service(); return;");
        return ok_;
    case Op::rts: {
        const unsigned r = i.reg;
        if (r == 7) {
            line("t = b.read16(static_cast<std::uint16_t>(r6 & 0xFFFE)); r6 = static_cast<std::uint16_t>(r6 + 2);");
        } else {
            line(std::format("t = r{0}; r{0} = b.read16(static_cast<std::uint16_t>(r6 & 0xFFFE)); r6 = static_cast<std::uint16_t>(r6 + 2);", r));
        }
        line("QL_SYNC(); c.r[7] = t; return;");
        return ok_;
    }
    case Op::ccc:
    case Op::scc: {
        const auto value = i.op == Op::scc ? "true" : "false";
        const unsigned mask = i.reg;
        if (mask & 8) line(std::format("n = {};", value));
        if (mask & 4) line(std::format("z = {};", value));
        if (mask & 2) line(std::format("v = {};", value));
        if (mask & 1) line(std::format("cf = {};", value));
        go_next();
        return ok_;
    }
    case Op::br:
        if (node.target) {
            if (node.target->pc <= at_.pc) service_point(node.target->pc);
            transfer(*node.target);
        } else {
            leave(hex(i.branch_target(at_.pc)));
        }
        return ok_;
    case Op::sob: {
        if (i.reg == 7) return fail("SOB PC is not supported");
        line(std::format("r{0} = static_cast<std::uint16_t>(r{0} - 1);", i.reg));
        line(std::format("if (r{} != 0) {{", i.reg));
        if (node.target) {
            service_point(node.target->pc);
            transfer(*node.target);
        } else {
            leave(hex(i.branch_target(at_.pc)));
        }
        line("}");
        go_next();
        return ok_;
    }
    case Op::jmp:
    case Op::jsr: {
        const bool is_call = i.op == Op::jsr;
        const auto& d = i.dst;
        std::string target;
        std::optional<Loc> static_target = node.target;
        if (d.absolute()) {
            // MAKE_EAW_IND(7): the address word is read as data, not fetched.
            line(std::format("static_cast<void>(b.read16({}));", hex(pc_)));
            pc_ = static_cast<std::uint16_t>(pc_ + 2);
            target = hex(d.word);
        } else if (d.relative()) {
            fetch();
            target = hex(d.word + pc_);
        } else {
            if (!effective_address(d, false, "ea")) return false;
            target = "ea";
            static_target.reset();
        }
        if (!is_call) {
            if (static_target) {
                if (static_target->pc <= at_.pc) service_point(static_target->pc);
                transfer(*static_target);
            } else {
                // JMP @X(Rn) through a table the analysis sized goes straight to its targets.
                if (node.flow == Flow::table) table_switch(node, target);
                leave(target);
            }
            return ok_;
        }
        // JSR: push the link register, link = PC, PC = ea.
        line("t = " + target + ";");
        line(std::format("r6 = static_cast<std::uint16_t>(r6 - 2); b.write16(static_cast<std::uint16_t>(r6 & 0xFFFE), {});", reg(i.reg)));
        if (i.reg != 7) line(std::format("r{} = {};", i.reg, hex(pc_)));
        call("t", static_target, pc_);
        go_next();
        return ok_;
    }
    default:
        break;
    }

    if (is_conditional(i.op)) {
        line(std::format("if ({}) {{", condition(i.op)));
        if (node.target) {
            if (node.target->pc <= at_.pc) service_point(node.target->pc);
            transfer(*node.target);
        } else {
            leave(hex(i.branch_target(at_.pc)));
        }
        line("}");
        go_next();
        return ok_;
    }

    // A write to PC is a jump: through a proven table, or computed.
    const bool writes_pc = i.operands >= 1 && i.dst.mode == t11::reg && i.dst.reg == 7 && i.op != Op::cmp &&
                           i.op != Op::bit && i.op != Op::tst && i.op != Op::mtps;
    const auto& s = i.src;
    const auto& d = i.dst;

    switch (i.op) {
    case Op::mov:
        if (!get_source(s, B, "src")) return false;
        flags_nz(B, "src");
        line("v = false;");
        if (d.mode == t11::reg) {
            if (d.reg == 7) {
                line(B ? "res = static_cast<std::uint16_t>(static_cast<std::int8_t>(src));" : "res = src;");
                break;
            }
            line(B ? std::format("r{} = static_cast<std::uint16_t>(static_cast<std::int8_t>(src & 0xFF));", d.reg)
                   : std::format("r{} = static_cast<std::uint16_t>(src);", d.reg));
        } else if (!put_dest(d, B, "src", true)) {
            return false;
        }
        break;
    case Op::cmp:
        if (!get_source(s, B, "src") || !get_dest(d, B)) return false;
        line("res = src - dst;");
        flags_nzvc(B, "src", "dst", "res");
        break;
    case Op::bit:
        if (!get_source(s, B, "src") || !get_dest(d, B)) return false;
        line("res = dst & src;");
        flags_nz(B, "res");
        line("v = false;");
        break;
    case Op::bic:
    case Op::bis:
        if (!get_source(s, B, "src") || !get_dest(d, B)) return false;
        line(i.op == Op::bic ? "res = dst & ~src;" : "res = dst | src;");
        flags_nz(B, "res");
        line("v = false;");
        if (!writes_pc) put_dest_ea(d, B, "res");
        break;
    case Op::add:
    case Op::sub:
        if (!get_source(s, false, "src") || !get_dest(d, false)) return false;
        line(i.op == Op::add ? "res = dst + src;" : "res = dst - src;");
        flags_nzvc(false, "src", "dst", "res");
        if (!writes_pc) put_dest_ea(d, false, "res");
        break;
    case Op::xor_:
        line(std::format("src = {};", reg(i.reg)));
        if (!get_dest(d, false)) return false;
        line("res = dst ^ src;");
        flags_nz(false, "res");
        line("v = false;");
        if (!writes_pc) put_dest_ea(d, false, "res");
        break;
    case Op::clr:
        if (d.mode == t11::reg) {
            if (!writes_pc) put_dest_ea(d, B, "0");
            else line("res = 0;");
        } else if (!put_dest(d, B, "0", true)) {
            return false;
        }
        line("n = false; z = true; v = false; cf = false;");
        break;
    case Op::sxt:
        line("res = n ? 0xFFFF : 0; z = !n; v = false;");
        if (d.mode == t11::reg) {
            if (!writes_pc) put_dest_ea(d, false, "res");
        } else if (!put_dest(d, false, "res", true)) {
            return false;
        }
        break;
    case Op::mfps:
        line("t = static_cast<std::uint8_t>((c.psw & 0xF0) | (n << 3) | (z << 2) | (v << 1) | static_cast<int>(cf));");
        line("n = (t >> 7) & 1; z = (t & 0xFF) == 0; v = false;");
        if (d.mode == t11::reg) {
            if (!writes_pc) line(std::format("r{} = static_cast<std::uint16_t>(static_cast<std::int8_t>(t));", d.reg));
            else line("res = static_cast<std::uint16_t>(static_cast<std::int8_t>(t));");
        } else if (!put_dest(d, true, "t", false)) {
            return false;
        }
        break;
    case Op::mtps:
        if (!get_dest(d, true)) return false;
        // PSW = (PSW & ~0xEF) | (dst & 0xEF): T is kept.
        line("c.psw = static_cast<std::uint8_t>((c.psw & 0x10) | (dst & 0xE0)); n = (dst >> 3) & 1; z = (dst >> 2) & 1; v = (dst >> 1) & 1; cf = dst & 1;");
        if (fall) {
            service_point(next_pc, true);   // MAME checks interrupts at once after MTPS
        }
        break;
    default: {
        // Read-modify-write one-operand instructions.
        if (!get_dest(d, B)) return false;
        const int sign = B ? 0x80 : 0x8000;
        const int width = B ? 8 : 16;
        switch (i.op) {
        case Op::com:
            line("res = ~dst;");
            flags_nz(B, "res");
            line("v = false; cf = true;");
            break;
        case Op::inc:
            line("res = dst + 1;");
            flags_nz(B, "res");
            line(std::format("v = dst == {};", sign - 1));
            break;
        case Op::dec:
            line("res = dst - 1;");
            flags_nz(B, "res");
            line(std::format("v = dst == {};", sign));
            break;
        case Op::neg:
            line("res = -dst;");
            flags_nz(B, "res");
            line(std::format("v = dst == {}; cf = res != 0;", sign));
            break;
        case Op::adc:
        case Op::sbc:
            line("src = cf;");
            line(i.op == Op::adc ? "res = dst + src;" : "res = dst - src;");
            flags_nzvc(B, "src", "dst", "res");
            break;
        case Op::tst:
            flags_nz(B, "dst");
            line("v = false; cf = false;");
            break;
        case Op::ror:
            line(std::format("res = (dst >> 1) | (static_cast<int>(cf) << {});", width - 1));
            flags_nz(B, "res");
            line("cf = dst & 1; v = n != cf;");
            break;
        case Op::rol:
            line("res = (dst << 1) | static_cast<int>(cf);");
            flags_nz(B, "res");
            line(std::format("cf = (dst >> {}) & 1; v = n != cf;", width - 1));
            break;
        case Op::asr:
            line(std::format("res = (dst >> 1) | (dst & {});", sign));
            flags_nz(B, "res");
            line("cf = dst & 1; v = n != cf;");
            break;
        case Op::asl:
            line("res = dst << 1;");
            flags_nz(B, "res");
            line(std::format("cf = (dst >> {}) & 1; v = n != cf;", width - 1));
            break;
        case Op::swab:
            line("res = ((dst >> 8) & 0xFF) + (dst << 8);");
            flags_nz(true, "res");
            line("v = false; cf = false;");
            break;
        default:
            return fail("no lifter for this instruction");
        }
        if (i.op != Op::tst && !writes_pc) put_dest_ea(d, B, "res");
        break;
    }
    }

    if (writes_pc) {
        // The new PC: through a table the analysis sized, or computed.
        line("t = static_cast<std::uint16_t>(res);");
        if (node.flow == Flow::table) table_switch(node, "t");
        leave("t");
        return ok_;
    }
    go_next();
    return ok_;
}

std::string FunctionWriter::write(std::vector<std::pair<Loc, unsigned>>& entries, std::size_t& instructions) {
    const auto& nodes = program_.nodes();
    // Members: everything reachable from the entry without following calls.
    std::vector<Loc> work{entry_};
    while (!work.empty()) {
        const auto at = work.back();
        work.pop_back();
        if (!nodes.contains(at) || !members_.insert(at).second) {
            continue;
        }
        for (const auto& n : nodes.at(at).next) {
            work.push_back(n);
        }
    }
    instructions += members_.size();

    std::string code;
    for (const auto& member : members_) {
        at_ = member;
        body_.clear();
        instruction(nodes.at(member));
        code += "@@" + label(member) + "@@\n" + body_;
    }
    // Entries dispatch may use: the function's start, each return site in it,
    // and each place a service point resumes at. An interrupt handler can
    // return somewhere other than where it interrupted (APB's switches
    // tasks), so code interrupted at a service point must be re-enterable.
    std::set<Loc> sites;
    for (const auto& member : members_) {
        if (member != entry_ && program_.return_sites().contains(member)) sites.insert(member);
    }
    // A table jump can service the board (and so be interrupted) before it
    // goes to its target, which is then where the code resumes.
    for (const auto& member : members_) {
        const auto& node = program_.nodes().at(member);
        if (node.flow != Flow::table) continue;
        for (const auto& target : node.next) {
            if (target != entry_ && members_.contains(target)) sites.insert(target);
        }
    }
    for (const auto& [pc, where] : resumes_) {
        const Loc resume{pc, static_cast<std::uint8_t>(Loc{pc, 0}.window() == where.window() ? where.bank : 0)};
        if (resume != entry_ && members_.contains(resume)) sites.insert(resume);
    }
    std::vector<Loc> resume_sites(sites.begin(), sites.end());
    for (const auto& site : resume_sites) referenced_.insert(site);
    referenced_.insert(entry_);
    // Emit only labels something jumps to.
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
        for (const auto& r : referenced_) {
            used = used || label(r) == name;
        }
        if (used) text += name + ":\n";
        pos = end + 3;
    }

    // Scratch locals, declared only where the body uses them (MSVC warns
    // about unused ones even when marked [[maybe_unused]]).
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
    f += "    rt::Cpu& c = b.cpu;\n"
         "    std::uint16_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0;\n"
         "    bool n = false, z = false, v = false, cf = false;\n"
         "    std::int32_t ic = 0;\n";
    for (const auto* name : {"src", "dst", "res"}) {
        if (uses(name)) f += std::format("    int {} = 0;\n", name);
    }
    if (uses("ea")) f += "    std::uint16_t ea = 0;\n";
    if (uses("t")) f += "    std::uint16_t t = 0;\n";
    if (!resumes_.empty()) f += "    unsigned resume = 0;\n";
    f += "    QL_LOAD();\n";
    if (resume_sites.empty()) {
        f += std::format("    static_cast<void>(entry);\n    goto {};\n", label(entry_));
    } else {
        f += "    switch (entry) {\n";
        for (std::size_t k = 0; k < resume_sites.size(); ++k) {
            f += std::format("    case {}: goto {};\n", k + 1, label(resume_sites[k]));
            entries.emplace_back(resume_sites[k], static_cast<unsigned>(k + 1));
        }
        f += std::format("    default: goto {};\n    }}\n", label(entry_));
    }
    f += text;
    if (!resumes_.empty()) {
        // Board service: write back, let the board run (it may take an
        // interrupt), and resume only if control came back to the same place.
        f += "service:\n    QL_SYNC();\n    switch (resume) {\n";
        for (std::size_t k = 0; k < resumes_.size(); ++k) {
            const auto& [pc, where] = resumes_[k];
            const int w = where.window();
            f += std::format("    case {}: c.r[7] = {}; b.service(); if (c.r[7] != {}{}) return; QL_LOAD(); goto S{};\n", k,
                             hex(pc), hex(pc), w >= 0 ? std::format(" || b.bank[{}] != {}", w, where.bank) : "", k);
        }
        f += "    default: return;\n    }\n";
    }
    f += "}\n\n";
    return f;
}

} // namespace

std::string function_name(Loc entry) {
    return entry.pc >= 0x8000 ? std::format("fn_{:04X}", entry.pc) : std::format("fn_{:04X}_b{}", entry.pc, entry.bank);
}

Result lift(const cfg::Program& program, const Options& options) {
    Result result;
    std::set<Loc> functions;
    for (const auto& f : program.functions()) {
        if (program.nodes().contains(f)) functions.insert(f);
    }
    result.functions = functions.size();

    static constexpr const char* prelude =
        "// Generated by qilin lift from the APB rev 7 ROMs. Not for the repository.\n"
        "#include \"qilin_board.hpp\"\n"
        "#include \"lifted.hpp\"\n\n"
        "#if defined(__GNUC__)\n"
        "#pragma GCC diagnostic ignored \"-Wunused-label\"\n"
        "#endif\n\n"
        "#define QL_LOAD() (r0 = c.r[0], r1 = c.r[1], r2 = c.r[2], r3 = c.r[3], r4 = c.r[4], r5 = c.r[5], r6 = c.r[6], \\\n"
        "    n = (c.psw & 8) != 0, z = (c.psw & 4) != 0, v = (c.psw & 2) != 0, cf = (c.psw & 1) != 0, ic = b.icount)\n"
        "#define QL_SYNC() (c.r[0] = r0, c.r[1] = r1, c.r[2] = r2, c.r[3] = r3, c.r[4] = r4, c.r[5] = r5, c.r[6] = r6, \\\n"
        "    c.psw = static_cast<std::uint8_t>((c.psw & 0xF0) | (n << 3) | (z << 2) | (v << 1) | static_cast<int>(cf)), b.icount = ic)\n\n"
        "namespace qilin::gen {\n\nusing rt::Board;\n\n";

    std::vector<std::pair<Loc, std::pair<std::string, unsigned>>> entries;   // (where, (function, index))
    std::vector<std::string> bodies;
    for (const auto& f : functions) {
        std::vector<std::pair<Loc, unsigned>> resumes;
        FunctionWriter writer(program, f, functions, options, result.failures);
        bodies.push_back(writer.write(resumes, result.instructions));
        entries.push_back({f, {function_name(f), 0}});
        for (const auto& [where, index] : resumes) {
            entries.push_back({where, {function_name(f), index}});
        }
    }
    // One entry per place: a function start wins over a resume point in another function.
    std::ranges::stable_sort(entries, [](const auto& a, const auto& b) {
        return std::pair(a.first.pc, a.first.bank) < std::pair(b.first.pc, b.first.bank) ||
               (a.first == b.first && a.second.second < b.second.second);
    });
    const auto [first, last] = std::ranges::unique(entries, [](const auto& a, const auto& b) { return a.first == b.first; });
    entries.erase(first, last);

    std::string header = "// Generated by qilin lift. Not for the repository.\n#pragma once\n#include \"t11_rt.hpp\"\n\n"
                         "namespace qilin::gen {\n\nusing rt::Board;\n\n";
    for (const auto& f : functions) {
        header += std::format("void {}(Board& b, unsigned entry);\n", function_name(f));
    }
    header += "\n} // namespace qilin::gen\n";
    result.files.push_back({"lifted.hpp", header});

    for (std::size_t k = 0; k * options.functions_per_file < bodies.size(); ++k) {
        std::string text = prelude;
        for (std::size_t j = k * options.functions_per_file; j < std::min(bodies.size(), (k + 1) * options.functions_per_file); ++j) {
            text += bodies[j];
        }
        text += "} // namespace qilin::gen\n";
        result.files.push_back({std::format("lifted_{}.cpp", k), text});
    }

    std::string dispatch =
        "// Generated by qilin lift. Not for the repository.\n"
        "#include \"qilin_board.hpp\"\n#include \"lifted.hpp\"\n\n#include <algorithm>\n\n"
        "namespace qilin::rt {\n\nconst Entry lifted_entries[] = {\n";
    for (const auto& [where, fn] : entries) {
        dispatch += std::format("    {{{}, {}, &gen::{}, {}}},\n", hex(where.pc), where.bank, fn.first, fn.second);
    }
    dispatch += std::format("}};\nconst unsigned lifted_entry_count = {};\n\n", entries.size());
    dispatch +=
        "bool dispatch(Board& b) {\n"
        "    const std::uint16_t pc = b.cpu.r[7];\n"
        "    if (pc < 0x4000) return false;\n"
        "    const auto bank = static_cast<std::uint8_t>(pc >= 0x8000 ? 0 : b.bank[pc >= 0x6000 ? 1 : 0]);\n"
        "    const auto* end = lifted_entries + lifted_entry_count;\n"
        "    const auto* e = std::lower_bound(lifted_entries, end, pc, [&](const Entry& x, std::uint16_t key) {\n"
        "        return x.pc < key || (x.pc == key && x.bank < bank);\n"
        "    });\n"
        "    if (e == end || e->pc != pc || e->bank != bank) return false;\n"
        "    e->fn(b, e->index);\n"
        "    return true;\n"
        "}\n\n} // namespace qilin::rt\n";
    result.files.push_back({"lifted_dispatch.cpp", dispatch});
    return result;
}

} // namespace qilin::lift
