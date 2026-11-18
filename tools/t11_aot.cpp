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
    bool runtime_rom_read{};
    std::optional<Address> return_pc{};
    std::optional<unsigned> return_register{};
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
    if ((address & 0xffe1u) == 0x15a0) {
        return std::format("board.sound.write_reset(static_cast<std::uint8_t>({}))", value);
    }
    if ((address & 0xff81u) == 0x1680) {
        return std::format("board.sound.write_command(static_cast<std::uint8_t>({}))", value);
    }
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
    if ((address & 0xfc00u) == 0x1c00) {
        return std::format("(board.video.observe(0x{:04X}), board.sound.read_reply(board.interrupts))", address);
    }
    if (const auto value = image.rom_data_word(where, address)) {
        return std::format("board.read_rom_word(0x{:04X}, 0x{:04X})", address, *value);
    }
    require_data_address(where, address, "read");
    return std::format("board.read_data_word(0x{:04X})", address);
}

enum class WordOp { move, compare, test_bits, clear_bits, set_bits, add, subtract };

bool register_memory(unsigned operand) {
    const auto mode = operand & 070;
    return (operand & 7) < 7 && (mode == 010 || mode == 020 || mode == 040 || mode == 060 || mode == 070);
}

bool has_index_word(unsigned operand) {
    return (operand & 060) == 060;
}

std::string checked_word(std::string& setup, Location where, std::string_view address, std::string_view name) {
    setup += std::format("        const auto {0} = read_bus_word(board, {1}, Banks{{{2}, {3}}});\n"
                         "        if (!{0}) return {{StopReason::memory_read, 0x{4:04X}, Banks{{{2}, {3}}}, 0, {1}}};\n",
                         name, address, where.banks[0], where.banks[1], where.pc);
    return std::format("*{}", name);
}

std::string register_operand(std::string& setup, Location where, unsigned operand,
                             std::uint16_t displacement, std::string_view role, bool write) {
    const auto address = std::format("{}_address", role);
    const auto number = operand & 7;
    const auto base = std::format("registers[{}]", number);
    if ((operand & 070) == 040) {
        setup += std::format("        {} -= 2;\n", base);
    }
    auto effective = displacement == 0 ? base
        : std::format("static_cast<std::uint16_t>({} + 0x{:04X}u)", base, displacement);
    if ((operand & 070) == 070) {
        const auto pointer = std::format("{}_pointer", role);
        setup += std::format("        const auto {} = word_address({});\n", pointer, effective);
        effective = checked_word(setup, where, pointer, pointer + "_value");
    }
    setup += std::format("        const auto {} = word_address({});\n", address, effective);
    if ((operand & 070) == 020) {
        setup += std::format("        registers[{0}] = static_cast<std::uint16_t>(registers[{0}] + 2u);\n", number);
    }
    if (!write) {
        return checked_word(setup, where, address, address + "_value");
    }
    setup += std::format("        if (!is_system2_data_word({0})) return {{StopReason::memory_write, 0x{1:04X}, Banks{{{2}, {3}}}, 0, {0}}};\n",
                         address, where.pc, where.banks[0], where.banks[1]);
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
        return register_operand(setup, where, operand, displacement, role, write);
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
        auto instruction = observed_instruction(next, words, setup, operation);
        instruction.runtime_rom_read = indirect;
        return instruction;
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
            src = memory_address(source, "source", false);
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
            dst = reads_only ? dst_address : std::format("board.read_data_word({})", dst_address);
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
                        auto instruction = dynamic_bank_switch(next, words, *window, setup, value, indirect);
                        instruction.runtime_rom_read = indirect;
                        return instruction;
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

Instruction decode_byte(const aot::System2Image& image, Location where, std::uint16_t opcode) {
    const bool clear = (opcode & 0177700) == 0105000;
    const bool bit_set = (opcode & 0170000) == 0150000;
    const auto source = (opcode >> 6) & 077;
    const auto destination = opcode & 077;
    unsigned words = 1;
    bool runtime_rom = false;
    std::optional<Address> absolute_address;
    std::string setup = "        auto registers = s.r;\n";
    const auto extension = [&] {
        const auto offset = 2 * words++;
        setup += observe(where.advance(static_cast<int>(offset)).pc);
        return image.word(where, offset);
    };
    const auto address = [&](unsigned operand, std::string_view name) {
        const auto number = operand & 7;
        const auto mode = operand >> 3;
        const auto step = number == 6 ? 2 : 1;
        const auto base = std::format("registers[{}]", number);
        std::string effective = base;
        if (operand == absolute_pc) {
            absolute_address = extension();
            effective = std::format("0x{:04X}", *absolute_address);
        } else if (number == 7 || (mode != 1 && mode != 2 && mode != 4 && mode != 6)) {
            unsupported(where, opcode, true);
        } else if (mode == 4) {
            setup += std::format("        {} -= {};\n", base, step);
        } else if (mode == 6) {
            effective += std::format(" + 0x{:04X}u", extension());
        }
        setup += std::format("        const auto {} = static_cast<std::uint16_t>({});\n", name, effective);
        if (mode == 2) {
            setup += std::format("        {} += {};\n", base, step);
        }
    };
    const auto failure = [&](std::string_view reason, std::string_view addr) {
        return std::format("return {{StopReason::{}, 0x{:04X}, Banks{{{}, {}}}, 0, {}}};\n",
                           reason, where.pc, where.banks[0], where.banks[1], addr);
    };

    std::string source_value = "0";
    if (!clear) {
        if (source < 7) {
            source_value = std::format("registers[{}]", source);
        } else if (source == immediate_pc) {
            source_value = std::format("0x{:02X}", extension() & 0xffu);
        } else {
            address(source, "src_address");
            runtime_rom = true;
            source_value = checked_word(setup, where, "src_address", "source_word") + " >> ((src_address & 1u) * 8)";
        }
        setup += std::format("        const auto source_byte = static_cast<std::uint8_t>({});\n", source_value);
    }

    std::string dst_word;
    std::string shift;
    if (destination < 7) {
        dst_word = std::format("registers[{}]", destination);
        shift = "0";
    } else {
        address(destination, "dst_address");
        if (destination == absolute_pc && !bit_set && !clear) {
            auto write = device_write(*absolute_address, "ops::move_byte(s.flags, source_byte)");
            if (!write.empty()) {
                setup += "        board.video.observe(dst_address);\n        board.video.observe(dst_address);\n";
                auto instruction = observed_instruction(where.advance(static_cast<int>(2 * words)), words, setup,
                    write + ";\n        s.r = registers");
                instruction.runtime_rom_read = runtime_rom;
                return instruction;
            }
        }
        setup += "        if (!is_system2_data_word(dst_address)) " + failure("memory_write", "dst_address");
        setup += "        const auto aligned = word_address(dst_address);\n"
                 "        const auto old_word = board.read_data_word(aligned);\n";
        dst_word = "old_word";
        shift = "((dst_address & 1u) * 8)";
    }
    std::string result;
    if (clear) {
        result = "ops::clear(s.flags)";
    } else {
        const auto byte = bit_set ? std::format("static_cast<std::uint8_t>(source_byte | ({} >> {}))", dst_word, shift)
                                  : "source_byte";
        result = "ops::move_byte(s.flags, " + byte + ")";
    }
    setup += "        const auto result = " + result + ";\n";
    if (destination < 7 && !clear && !bit_set) {
        setup += "        " + dst_word + " = result;\n";
    } else {
        const auto merged = std::format("static_cast<std::uint16_t>(({0} & ~(0xffu << {1})) | ((result & 0xffu) << {1}))",
                                        dst_word, shift);
        setup += destination < 7 ? "        " + dst_word + " = " + merged + ";\n"
                                 : "        board.write_data_word(aligned, " + merged + ");\n";
    }
    auto instruction = observed_instruction(where.advance(static_cast<int>(2 * words)), words, setup, "s.r = registers");
    instruction.runtime_rom_read = runtime_rom;
    return instruction;
}

template <typename Image>
Instruction decode(const Image& image, Location where, const aot::JumpTables& tables) {
    const auto opcode = image.word(where);
    const auto next = where.advance(2);

    if constexpr (Image::banked) {
        const auto link = (opcode >> 6) & 7;
        if (link != 6 && ((opcode & 0177077) == 0004037 || (opcode & 0177077) == 0004067)) {
            const auto operand = image.word(where, 2);
            const auto resume = where.advance(4);
            const auto target = (opcode & 077) == absolute_pc ? where.at(operand) : resume.advance(static_cast<std::int16_t>(operand));
            auto setup = observe(next.pc);
            setup += std::format("        const auto sp = static_cast<std::uint16_t>(s.r[6] - 2u);\n"
                "        const auto address = word_address(sp);\n"
                "        if (!is_system2_data_word(address)) return {{StopReason::memory_write, 0x{:04X}, Banks{{{}, {}}}, 0, address}};\n"
                "        board.write_data_word(address, {});\n", where.pc, where.banks[0], where.banks[1],
                link == 7 ? std::format("0x{:04X}", resume.pc) : reg(link));
            if (link != 7) {
                setup += std::format("        {} = 0x{:04X};\n", reg(link), resume.pc);
            }
            auto instruction = observed_instruction(target, 2, setup, "s.r[6] = sp");
            instruction.return_pc = resume.pc;
            return instruction;
        }
        if ((opcode & 0177770) == 0000200 && (opcode & 7) != 6) {
            Instruction instruction{1, "    {\n        const auto address = word_address(s.r[6]);\n", {}};
            checked_word(instruction.body, where, "address", "saved");
            const auto number = opcode & 7;
            instruction.body += std::format("        const auto target = {};\n        switch (target) {{\n",
                                            number == 7 ? "*saved" : reg(number));
            instruction.runtime_rom_read = true;
            instruction.return_register = number;
            return instruction;
        }
        if ((opcode & 0170000) == 0110000 || (opcode & 0170000) == 0150000 ||
            ((opcode & 0177700) == 0105000 && (opcode & 077) != absolute_pc && (opcode & 077) > 6)) {
            return decode_byte(image, where, opcode);
        }
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
            const auto address = register_operand(setup, where, destination, indexed_offset, "destination", true);
            setup += std::format("        board.video.observe({});\n", address);
            auto instruction = observed_instruction(where.advance(static_cast<int>(2 * words)), words, setup,
                std::format("board.write_data_word({}, ops::clear(s.flags));\n        s.r = registers", address));
            instruction.runtime_rom_read = (destination & 070) == 070;
            return instruction;
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
    std::set<Address> continuations;
    std::set<qilin::t11::Banks> returning_banks;
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
            // A callee can return under a different bank mapping. Discover a continuation
            // in every returning context, regardless of which site was encountered first.
            if (instruction.return_pc && continuations.insert(*instruction.return_pc).second) {
                for (const auto banks : returning_banks) {
                    pending.push_back({*instruction.return_pc, banks});
                }
            }
            if (instruction.return_register && returning_banks.insert(where.banks).second) {
                for (const auto pc : continuations) {
                    pending.push_back(where.at(pc));
                }
            }
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
    for (auto& [where, instruction] : decoded) {
        if (instruction.return_register) {
            const auto restore = *instruction.return_register == 7 ? "" : reg(*instruction.return_register) + " = *saved; ";
            for (const auto pc : continuations) {
                instruction.body += std::format("        case 0x{:04X}: {}s.r[6] += 2; {}", pc, restore, jump(where.at(pc)));
            }
            instruction.body += std::format("        default: return {{StopReason::return_target, 0x{:04X}, Banks{{{}, {}}}, 0, target}};\n"
                                            "        }}\n    }}\n", where.pc, where.banks[0], where.banks[1]);
        }
    }
    return decoded;
}

void emit_bus_reader(std::ostream& out, const aot::System2Image& image) {
    out << "[[maybe_unused]] static std::optional<std::uint16_t> read_bus_word(System2State& board, std::uint16_t address, Banks banks) {\n"
           "    address = word_address(address);\n"
           "    if (is_system2_data_word(address)) return board.read_data_word(address);\n"
           "    if ((address & 0xfc00u) == 0x1c00) {\n"
           "        board.video.observe(address);\n"
           "        return board.sound.read_reply(board.interrupts);\n"
           "    }\n"
           "    if (address < 0x4000) return std::nullopt;\n"
           "    const auto offset = physical_rom_offset(address, banks) / 2;\n";
    const auto& words = image.physical_words();
    for (std::size_t index = 0; index < words.size();) {
        if (!words[index]) {
            ++index;
            continue;
        }
        const auto start = index;
        while (index < words.size() && words[index]) {
            ++index;
        }
        out << std::format("    if (offset >= {} && offset < {}) {{\n        static constexpr std::uint16_t data[] = {{", start, index);
        for (auto i = start; i < index; ++i) {
            if ((i - start) % 16 == 0) {
                out << "\n            ";
            }
            out << std::format("0x{:04X}, ", *words[i]);
        }
        out << std::format("\n        }};\n        return board.read_rom_word(address, data[offset - {}]);\n    }}\n", start);
    }
    out << "    return std::nullopt;\n}\n\n";
}

void emit(std::ostream& out, Location entry, const Program& decoded, const aot::System2Image* image) {
    const bool banked = image != nullptr;
    out << "#include <qilin/t11.hpp>\n\n"
           "namespace qilin::t11 {\n\n";
    if (banked) {
        for (const auto& item : decoded) {
            if (item.second.runtime_rom_read) {
                emit_bus_reader(out, *image);
                break;
            }
        }
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

void write_program(const std::filesystem::path& path, Location entry, const Program& decoded, const aot::System2Image* image) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot open C++ output");
    }
    emit(out, entry, decoded, image);
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
                write_program(output, entry, decoded, &image);
            }
            std::cout << (inspect ? "Analyzed " : "Translated ") << decoded.size() << " reachable T-11 locations.\n";
        } else {
            const Location entry{aot::parse_address(argv[2])};
            const std::filesystem::path output = argv[3];
            const aot::FlatImage image(argv[1], entry.pc, output);
            const auto decoded = analyze(image, entry, {});
            write_program(output, entry, decoded, nullptr);
            std::cout << "Translated " << decoded.size() << " reachable T-11 instructions.\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "apb-t11-aot: " << error.what() << '\n';
        return 1;
    }
}
