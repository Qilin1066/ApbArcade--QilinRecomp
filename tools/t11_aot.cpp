#include "t11_image.hpp"

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace aot = qilin::t11::aot;
using aot::Address;
using aot::Location;

constexpr unsigned immediate_pc = 027;
constexpr unsigned absolute_pc = 037;
constexpr unsigned relative_pc = 067;

struct Instruction {
    unsigned words = 1;
    std::string body;
    std::vector<Location> successors;
    std::vector<Location> jump_table_words{};
    std::optional<unsigned> switched_window{};
};

std::string label(Location where) {
    return std::format("pc_{:04X}_bank_{:02X}_{:02X}", where.pc, where.banks[0], where.banks[1]);
}

std::string jump(Location where) {
    return std::format("goto {};\n", label(where));
}

std::string stop(std::string_view reason, Location where) {
    return std::format("return {{StopReason::{}, 0x{:04X}, Banks{{{}, {}}}}};\n",
                       reason, where.pc, where.banks[0], where.banks[1]);
}

std::string reg(unsigned number) {
    return std::format("s.r[{}]", number);
}

[[noreturn]] void unsupported(Location where, std::uint16_t opcode, bool banked) {
    throw std::runtime_error(std::format("unsupported T-11 instruction 0x{:04X} at {}",
                                         opcode, aot::describe(where, banked)));
}

std::string_view branch_condition(std::uint16_t opcode) {
    switch (opcode & 0177400) {
    case 0001000: return "!s.flags.z";                              // BNE
    case 0001400: return "s.flags.z";                               // BEQ
    case 0002000: return "s.flags.n == s.flags.v";                  // BGE
    case 0002400: return "s.flags.n != s.flags.v";                  // BLT
    case 0003000: return "!s.flags.z && s.flags.n == s.flags.v";    // BGT
    case 0003400: return "s.flags.z || s.flags.n != s.flags.v";     // BLE
    case 0100000: return "!s.flags.n";                              // BPL
    case 0100400: return "s.flags.n";                               // BMI
    case 0101000: return "!s.flags.c && !s.flags.z";                 // BHI
    case 0101400: return "s.flags.c || s.flags.z";                   // BLOS
    case 0102000: return "!s.flags.v";                              // BVC
    case 0102400: return "s.flags.v";                               // BVS
    case 0103000: return "!s.flags.c";                              // BCC
    case 0103400: return "s.flags.c";                               // BCS
    default: return {};
    }
}

Instruction branch(Location next, Location target, std::string_view condition) {
    return {
        1,
        std::format("    if ({}) {}    {}", condition, jump(target), jump(next)),
        {target, next}
    };
}

Instruction sequential(Location next, unsigned words, std::string_view operation) {
    return {words, std::format("    {};\n    {}", operation, jump(next)), {next}};
}

std::string device_write(Address address, std::string_view value) {
    using qilin::system2::IrqWrite;
    if (qilin::system2::is_watchdog_write(address)) {
        return std::format("{};\n        board.watchdog.strobe()", value);
    }
    std::string_view source;
    switch (qilin::system2::irq_write_port(address)) {
    case IrqWrite::enable: return std::format("board.interrupts.set_enable(static_cast<std::uint8_t>({}))", value);
    case IrqWrite::ack_sound_command: source = "sound_command_read"; break;
    case IrqWrite::ack_scanline: source = "scanline"; break;
    case IrqWrite::ack_vblank: source = "vblank"; break;
    case IrqWrite::unmapped: return {};
    }
    return std::format("{};\n        board.interrupts.acknowledge(system2::IrqSource::{})", value, source);
}

std::string observe(Address address) {
    return std::format("        board.video.observe(0x{:04X});\n", address);
}

Instruction observed_instruction(Location next, unsigned words, std::string_view setup,
                                 std::string_view operation) {
    return {words, std::format("    {{\n{}        {};\n    }}\n    {}", setup, operation, jump(next)), {next}};
}

Instruction dynamic_bank_switch(Location next, unsigned words, unsigned window, std::string_view setup,
                                std::string_view value, bool indirect) {
    Instruction result{words, "    {\n", {}, {}, window};
    result.body += setup;
    result.body += std::format("        const auto bank = system2::select_rom_bank({});\n", value);
    if (indirect) {
        result.body += "        s.r = registers;\n";
    }
    result.body += "        switch (bank) {\n";
    for (unsigned bank = 0; bank < qilin::system2::rom_bank_count; ++bank) {
        next.banks[window] = static_cast<std::uint8_t>(bank);
        result.successors.push_back(next);
        const auto label_text = bank + 1 == qilin::system2::rom_bank_count ? "default" : std::format("case {}", bank);
        result.body += std::format("        {}: {}", label_text, jump(next));
    }
    result.body += "        }\n    }\n";
    return result;
}

void require_data_address(Location from, Address address, std::string_view access, bool byte = false) {
    if (!qilin::t11::is_system2_data_word(address)) {
        throw std::runtime_error(std::format("unsupported System 2 {} {} at 0x{:04X} from {}",
                                             byte ? "byte" : "word", access, address, aot::describe(from, true)));
    }
}

std::string read_absolute(const aot::System2Image& image, Location where, Address address) {
    address = qilin::t11::word_address(address);
    if (const auto value = image.rom_data_word(where, address)) {
        return std::format("board.read_rom_word(0x{:04X}, 0x{:04X})", address, *value);
    }
    require_data_address(where, address, "read");
    return std::format("board.read_data_word(0x{:04X})", address);
}

enum class WordOp { move, compare, test_bits, clear_bits, set_bits, add, subtract };

bool register_memory(unsigned operand) {
    const auto mode = operand & 070;
    return (operand & 7) < 7 && (mode == 010 || mode == 020 || mode == 060 || mode == 070);
}

bool has_index_word(unsigned operand) {
    return (operand & 060) == 060;
}

std::string register_address(std::string& setup, Location where, unsigned operand,
                             std::uint16_t displacement, std::string_view role, bool write) {
    const auto address = std::format("{}_address", role);
    const auto number = operand & 7;
    const auto base = std::format("registers[{}]", number);
    auto effective = displacement == 0 ? base
        : std::format("static_cast<std::uint16_t>({} + 0x{:04X}u)", base, displacement);
    if ((operand & 070) == 070) {
        const auto pointer = std::format("{}_pointer", role);
        setup += std::format(
            "        const auto {0} = word_address({1});\n"
            "        if (!is_system2_data_word({0})) return {{StopReason::memory_read, 0x{2:04X}, Banks{{{3}, {4}}}, 0, {0}}};\n",
            pointer, effective, where.pc, where.banks[0], where.banks[1]);
        effective = std::format("board.read_data_word({})", pointer);
    }
    setup += std::format(
        "        const auto {0} = word_address({1});\n"
        "        if (!is_system2_data_word({0})) return {{StopReason::memory_{2}, 0x{3:04X}, Banks{{{4}, {5}}}, 0, {0}}};\n",
        address, effective, write ? "write" : "read", where.pc, where.banks[0], where.banks[1]);
    if ((operand & 070) == 020) {
        setup += std::format("        registers[{0}] = static_cast<std::uint16_t>(registers[{0}] + 2u);\n", number);
    }
    return address;
}

template <typename Image>
Instruction decode_word_op(const Image& image, Location where, std::uint16_t opcode, WordOp op) {
    unsigned words = 1;
    const auto source = (opcode >> 6) & 077;
    const auto destination = opcode & 077;
    const bool reads_only = op == WordOp::compare || op == WordOp::test_bits;
    const bool indirect = Image::banked && (register_memory(source) || register_memory(destination));
    std::string setup;
    if (indirect) {
        setup = "        auto registers = s.r;\n";
    }
    const auto extension = [&] {
        const auto offset = 2 * words;
        const auto value = image.word(where, offset);
        ++words;
        if constexpr (Image::banked) {
            setup += observe(where.advance(static_cast<int>(offset)).pc);
        }
        return value;
    };
    const auto memory_address = [&](unsigned operand, std::string_view role, bool write) {
        // Addition in 16 bits gives the same address for signed and unsigned offsets.
        const auto displacement = has_index_word(operand) ? extension() : std::uint16_t{};
        return register_address(setup, where, operand, displacement, role, write);
    };
    const auto register_value = [&](unsigned number) {
        return indirect ? std::format("registers[{}]", number) : reg(number);
    };
    const auto finish = [&](Location next, std::string operation) -> Instruction {
        if constexpr (!Image::banked) {
            return sequential(next, words, operation);
        }
        if (indirect) {
            operation += ";\n        s.r = registers";
        }
        return observed_instruction(next, words, setup, operation);
    };

    std::optional<std::uint16_t> immediate;
    std::string src;
    if (source <= 6) {
        src = register_value(source);
    } else if (source == immediate_pc) {
        immediate = extension();
        src = std::format("0x{:04X}", *immediate);
    } else if constexpr (Image::banked) {
        if (source == absolute_pc) {
            src = read_absolute(image, where, extension());
        } else if (register_memory(source)) {
            const auto address = memory_address(source, "source", false);
            src = std::format("board.read_data_word({})", address);
        } else {
            unsupported(where, opcode, true);
        }
    } else {
        unsupported(where, opcode, false);
    }
    if constexpr (Image::banked) {
        // Read the source between its extension fetch and the destination's bus cycles.
        setup += std::format("        const auto source_value = {};\n", src);
        src = "source_value";
    }

    std::string dst;
    std::string dst_address;
    auto next = where.advance(static_cast<int>(2 * words));
    if (destination <= 6) {
        dst = register_value(destination);
    } else if (destination == immediate_pc && reads_only) {
        dst = std::format("0x{:04X}", extension());
        next = where.advance(static_cast<int>(2 * words));
    } else if constexpr (Image::banked) {
        if (register_memory(destination)) {
            dst_address = memory_address(destination, "destination", !reads_only);
            next = where.advance(static_cast<int>(2 * words));
            dst = std::format("board.read_data_word({})", dst_address);
        } else if (destination == absolute_pc) {
            const auto address = qilin::t11::word_address(extension());
            next = where.advance(static_cast<int>(2 * words));
            if (op == WordOp::move) {
                const auto value = std::format("ops::move(s.flags, {})", src);
                const auto write = device_write(address, value);
                if (!write.empty()) {
                    setup += observe(address) + observe(address);
                    return finish(next, write);
                }
                if (const auto window = aot::bank_selector(address)) {
                    setup += observe(address) + observe(address);
                    if (!immediate) {
                        return dynamic_bank_switch(next, words, *window, setup, value, indirect);
                    }
                    next.banks[*window] = qilin::system2::select_rom_bank(*immediate);
                    return finish(next, value);
                }
            }
            if (reads_only) {
                dst = read_absolute(image, where, address);
            } else {
                require_data_address(where, address, "write");
                dst_address = std::format("0x{:04X}", address);
                dst = std::format("board.read_data_word({})", dst_address);
            }
        } else {
            unsupported(where, opcode, true);
        }
    } else {
        unsupported(where, opcode, false);
    }

    const auto store = [&](std::string_view result) {
        if (dst_address.empty()) {
            return std::format("{} = {}", dst, result);
        }
        return std::format("board.write_data_word({}, {})", dst_address, result);
    };
    switch (op) {
    case WordOp::move:
        if (!dst_address.empty()) {
            // The memory form reads the destination on the bus before writing it.
            setup += std::format("        board.video.observe({});\n", dst_address);
        }
        return finish(next, store(std::format("ops::move(s.flags, {})", src)));
    case WordOp::compare:
        return finish(next, std::format("ops::subtract(s.flags, {}, {})", src, dst));
    case WordOp::test_bits:
        return finish(next, std::format("ops::move(s.flags, static_cast<std::uint16_t>({} & {}))", dst, src));
    case WordOp::clear_bits:
        return finish(next, store(std::format("ops::move(s.flags, static_cast<std::uint16_t>({} & ~{}))", dst, src)));
    case WordOp::set_bits:
        return finish(next, store(std::format("ops::move(s.flags, static_cast<std::uint16_t>({} | {}))", dst, src)));
    case WordOp::add:
        return finish(next, store(std::format("ops::add(s.flags, {}, {})", dst, src)));
    case WordOp::subtract:
        return finish(next, store(std::format("ops::subtract(s.flags, {}, {})", dst, src)));
    }
    unsupported(where, opcode, Image::banked);
}

Instruction decode_indexed_pc_add(const aot::System2Image& image, Location where, unsigned reg_number,
                                  unsigned count) {
    const auto base = image.word(where, 2);
    const auto next = where.advance(4);
    const auto end = static_cast<unsigned>(base) + 2u * count;
    if (base < 0x4000 || (base & 1u) != 0 || end > 0x10000u) {
        throw std::runtime_error("jump table must fit in aligned System 2 ROM at " + aot::describe(where, true));
    }

    Instruction result{2, {}, {}};
    std::set<Location> targets;
    std::string offsets;
    for (unsigned address = base; address < end; address += 2) {
        const auto slot = where.at(static_cast<Address>(address));
        const auto offset = image.word(slot);
        const auto target = next.advance(static_cast<std::int16_t>(offset));
        if (target.pc < 0x4000 || (target.pc & 1u) != 0 || (target.pc >= base && target.pc < end)) {
            throw std::runtime_error(std::format("invalid jump-table target 0x{:04X} from ROM slot 0x{:04X} at {}",
                                                 target.pc, slot.pc, aot::describe(where, true)));
        }
        static_cast<void>(image.word(target));
        if (!offsets.empty()) {
            offsets += ", ";
        }
        offsets += std::format("0x{:04X}", offset);
        result.jump_table_words.push_back(slot);
        targets.insert(target);
    }
    result.successors.assign(targets.begin(), targets.end());
    const auto invalid_slot = std::format(
        "return {{StopReason::jump_table, 0x{:04X}, Banks{{{}, {}}}, 0, table_address}};\n",
        where.pc, where.banks[0], where.banks[1]);
    result.body = "    {\n" + observe(where.advance(2).pc) +
        std::format("        const auto table_address = word_address(static_cast<std::uint16_t>({} + 0x{:04X}u));\n"
                    "        if (table_address < 0x{:04X} || table_address > 0x{:04X}) {}\n"
                    "        constexpr std::array<std::uint16_t, {}> offsets{{{}}};\n"
                    "        const auto offset = board.read_rom_word(table_address, offsets[(table_address - 0x{:04X}) / 2]);\n"
                    "        switch (ops::add(s.flags, 0x{:04X}, offset)) {{\n",
                    reg(reg_number), base, base, end - 2, invalid_slot, count, offsets, base, next.pc);
    for (const auto target : result.successors) {
        result.body += std::format("        case 0x{:04X}: {}", target.pc, jump(target));
    }
    result.body += "        default: " + invalid_slot + "        }\n    }\n";
    return result;
}

template <typename Image>
Instruction decode(const Image& image, Location where, const aot::JumpTables& tables) {
    const auto opcode = image.word(where);
    const auto next = where.advance(2);

    if constexpr (Image::banked) {
        const auto source = (opcode >> 6) & 077;
        if ((opcode & 0170077) == 0060007 && (source & 070) == 060 && (source & 7) < 7) {
            const auto table = tables.find(where.pc);
            if (table == tables.end()) {
                throw std::runtime_error("computed jump at " + aot::describe(where, true) +
                                         " needs a declaration in --jump-tables");
            }
            return decode_indexed_pc_add(image, where, source & 7, table->second);
        }
    }

    if (opcode == 0000000) {
        return {1, "    " + stop("halt", where), {}};
    }
    if (opcode == 0000240) {
        return {1, "    " + jump(next), {next}};
    }

    const auto displacement = static_cast<std::int8_t>(opcode & 0xff);
    const auto target = next.advance(2 * displacement);
    if ((opcode & 0177400) == 0000400) {
        return {1, "    " + jump(target), {target}};
    }
    if (const auto condition = branch_condition(opcode); !condition.empty()) {
        return branch(next, target, condition);
    }
    if ((opcode & 0177000) == 0077000) {
        const auto counter = (opcode >> 6) & 7;
        if (counter == 7) {
            unsupported(where, opcode, Image::banked);
        }
        const auto loop = next.advance(-2 * (opcode & 077));
        return branch(next, loop, "--" + reg(counter) + " != 0");
    }

    if (opcode == (0000100 | absolute_pc) || opcode == (0000100 | relative_pc)) {
        const auto operand = image.word(where, 2);
        const auto entry = opcode == (0000100 | absolute_pc) ? where.at(operand)
            : where.advance(4 + static_cast<std::int16_t>(operand));
        if constexpr (Image::banked) {
            return {2, observe(next.pc) + "    " + jump(entry), {entry}};
        }
        return {2, "    " + jump(entry), {entry}};
    }

    switch (opcode & 0170000) {
    case 0010000: return decode_word_op(image, where, opcode, WordOp::move);
    case 0020000: return decode_word_op(image, where, opcode, WordOp::compare);
    case 0030000: return decode_word_op(image, where, opcode, WordOp::test_bits);
    case 0040000: return decode_word_op(image, where, opcode, WordOp::clear_bits);
    case 0050000: return decode_word_op(image, where, opcode, WordOp::set_bits);
    case 0060000: return decode_word_op(image, where, opcode, WordOp::add);
    case 0160000: return decode_word_op(image, where, opcode, WordOp::subtract);
    }

    const auto destination = opcode & 077;
    const auto unary = opcode & 0177700;
    if constexpr (Image::banked) {
        if (unary == 0005000 && register_memory(destination)) {
            std::string setup = "        auto registers = s.r;\n";
            unsigned words = 1;
            std::uint16_t indexed_offset{};
            if (has_index_word(destination)) {
                indexed_offset = image.word(where, 2);
                setup += observe(next.pc);
                ++words;
            }
            const auto address = register_address(setup, where, destination, indexed_offset, "destination", true);
            setup += std::format("        board.video.observe({});\n", address);
            return observed_instruction(where.advance(static_cast<int>(2 * words)), words, setup,
                std::format("board.write_data_word({}, ops::clear(s.flags));\n        s.r = registers", address));
        }
        if (destination == absolute_pc && (unary == 0005000 || unary == 0105000)) {
            const bool byte = unary == 0105000;
            const auto operand = image.word(where, 2);
            const auto address = byte ? operand : qilin::t11::word_address(operand);
            auto setup = observe(next.pc);
            if (const auto write = device_write(address, "ops::clear(s.flags)"); !write.empty()) {
                setup += observe(address) + observe(address);
                return observed_instruction(where.advance(4), 2, setup, write);
            }
            require_data_address(where, address, "write", byte);
            const auto aligned = qilin::t11::word_address(address);
            if (byte) {
                const auto operation = std::format("board.write_data_word(0x{0:04X}, "
                    "ops::clear_byte(s.flags, board.read_data_word(0x{0:04X}), {1}))", aligned, address % 2 != 0);
                return observed_instruction(where.advance(4), 2, setup, operation);
            }
            setup += observe(address);
            return observed_instruction(where.advance(4), 2, setup,
                std::format("board.write_data_word(0x{:04X}, ops::clear(s.flags))", address));
        }
    }
    if (destination > 6) {
        unsupported(where, opcode, Image::banked);
    }
    const auto dst = reg(destination);
    switch (unary) {
    case 0005000: return sequential(next, 1, std::format("{} = ops::clear(s.flags)", dst));
    case 0105000: return sequential(next, 1, std::format("{} = ops::clear_byte(s.flags, {}, false)", dst, dst));
    case 0005200: return sequential(next, 1, std::format("{} = ops::increment(s.flags, {})", dst, dst));
    case 0005300: return sequential(next, 1, std::format("{} = ops::decrement(s.flags, {})", dst, dst));
    case 0005400: return sequential(next, 1, std::format("{} = ops::subtract(s.flags, 0, {})", dst, dst));
    case 0005700: return sequential(next, 1, std::format("ops::test(s.flags, {})", dst));
    case 0006300: return sequential(next, 1, std::format("{} = ops::add(s.flags, {}, {})", dst, dst, dst));
    }
    unsupported(where, opcode, Image::banked);
}

using Program = std::map<Location, Instruction>;

template <typename Image>
Program analyze(const Image& image, Location entry, const aot::JumpTables& tables,
                std::ostream* report = nullptr) {
    Program decoded;
    std::set<Location> operands;
    std::set<Location> table_words;
    std::vector<Location> pending{entry};
    const auto overlap = [](Location where) {
        return std::runtime_error("branch enters operand word at " + aot::describe(where, Image::banked));
    };

    try {
        while (!pending.empty()) {
            const auto where = pending.back();
            pending.pop_back();
            if (decoded.contains(where)) {
                continue;
            }
            if (operands.contains(where)) {
                throw overlap(where);
            }
            if (table_words.contains(where)) {
                throw std::runtime_error("branch enters jump table at " + aot::describe(where, Image::banked));
            }

            auto instruction = decode(image, where, tables);
            for (unsigned index = 1; index < instruction.words; ++index) {
                const auto operand = where.advance(static_cast<int>(2 * index));
                if (decoded.contains(operand) || table_words.contains(operand)) {
                    throw overlap(operand);
                }
                operands.insert(operand);
            }
            for (const auto slot : instruction.jump_table_words) {
                if (slot == where || decoded.contains(slot) || operands.contains(slot)) {
                    throw std::runtime_error("jump table overlaps an instruction at " + aot::describe(slot, Image::banked));
                }
                table_words.insert(slot);
            }
            if (report && !instruction.jump_table_words.empty()) {
                *report << std::format("Resolved {} ROM table entries to {} targets at {}.\n",
                                       instruction.jump_table_words.size(), instruction.successors.size(),
                                       aot::describe(where, Image::banked)) << std::flush;
            }
            if (report && instruction.switched_window) {
                *report << std::format("Expanded bank selector at {} to {} banks in window {}.\n",
                                       aot::describe(where, Image::banked), instruction.successors.size(),
                                       *instruction.switched_window) << std::flush;
            }
            pending.insert(pending.end(), instruction.successors.begin(), instruction.successors.end());
            decoded.emplace(where, std::move(instruction));
        }
    } catch (const std::runtime_error& error) {
        if constexpr (Image::banked) {
            throw std::runtime_error(std::format("{}\nAnalyzed {} reachable T-11 locations before this diagnostic.",
                                                 error.what(), decoded.size()));
        }
        throw;
    }
    return decoded;
}

void emit(std::ostream& out, Location entry, const Program& decoded, bool banked) {
    out << "#include <qilin/t11.hpp>\n\n"
           "namespace qilin::t11 {\n\n";
    if (banked) {
        out << "Stop run_system2(System2State& board, std::uint64_t budget) {\n"
               "    [[maybe_unused]] auto& s = board.cpu;\n"
               "    if (board.watchdog.reset_pending()) " << stop("watchdog", entry);
    } else {
        out << "Stop run([[maybe_unused]] State& s, std::uint64_t budget) {\n";
    }
    out << "    " << jump(entry);
    for (const auto& [where, instruction] : decoded) {
        out << label(where) << ":\n"
            << "    if (budget == 0) " << stop("budget", where);
        if (banked) {
            out << std::format("    if (const auto irq = accept_system2_irq(board, 0x{:04X}, Banks{{{}, {}}})) return *irq;\n",
                               where.pc, where.banks[0], where.banks[1]);
        }
        out << "    --budget;\n";
        if (banked) {
            out << std::format("    board.video.observe(0x{:04X});\n", where.pc);
        }
        out << instruction.body;
    }
    out << "}\n\n} // namespace qilin::t11\n";
}

void write_program(const std::filesystem::path& path, Location entry, const Program& decoded, bool banked) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot open C++ output");
    }
    emit(out, entry, decoded, banked);
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write C++ output");
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
    if (argc == 2 && mode == "--version") {
        std::cout << APB_VERSION << '\n';
        return 0;
    }
    const bool inspect = mode == "--inspect-system2";
    const bool banked = inspect || mode == "--system2";
    const auto required = inspect ? 5 : banked ? 6 : 4;
    const bool with_tables = banked && argc == required + 2 && std::string_view(argv[required]) == "--jump-tables";
    if (argc != required && !with_tables) {
        std::cerr << "Usage: apb-t11-aot <image.bin> <hex-origin> <output.cpp>\n"
                  << "       apb-t11-aot --system2 <rom-map> <rom-dir> <hex-entry> <output.cpp> [--jump-tables <map>]\n"
                  << "       apb-t11-aot --inspect-system2 <rom-map> <rom-dir> <hex-entry> [--jump-tables <map>]\n";
        return 2;
    }
    try {
        if (banked) {
            const Location entry{aot::parse_address(argv[4])};
            const std::filesystem::path output = inspect ? std::filesystem::path{} : argv[5];
            const aot::System2Image image(argv[2], argv[3], output);
            const auto tables = with_tables ? aot::read_jump_tables(argv[required + 1], output) : aot::JumpTables{};
            if (inspect) {
                std::cout << "Inspecting T-11 entry " << aot::describe(entry, true) << ".\n" << std::flush;
            }
            const auto decoded = analyze(image, entry, tables, inspect ? &std::cout : nullptr);
            if (!inspect) {
                write_program(output, entry, decoded, banked);
            }
            std::cout << (inspect ? "Analyzed " : "Translated ") << decoded.size() << " reachable T-11 locations.\n";
        } else {
            const Location entry{aot::parse_address(argv[2])};
            const std::filesystem::path output = argv[3];
            const aot::FlatImage image(argv[1], entry.pc, output);
            const auto decoded = analyze(image, entry, {});
            write_program(output, entry, decoded, banked);
            std::cout << "Translated " << decoded.size() << " reachable T-11 instructions.\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "apb-t11-aot: " << error.what() << '\n';
        return 1;
    }
}
