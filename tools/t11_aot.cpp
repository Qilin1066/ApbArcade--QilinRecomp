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

std::string ram_operand(Location from, Address address, std::string_view access, bool byte = false) {
    if (address >= qilin::t11::system2_ram_size || (!byte && address % 2 != 0)) {
        throw std::runtime_error(std::format("unsupported System 2 {} {} at 0x{:04X} from {}",
                                             byte ? "byte" : "word", access, address, aot::describe(from, true)));
    }
    return std::format("board.ram[0x{:03X}]", address / 2);
}

std::string read_absolute(const aot::System2Image& image, Location where, Address address) {
    if (const auto value = image.rom_data_word(where, address)) {
        return std::format("0x{:04X}", *value);
    }
    return ram_operand(where, address, "read");
}

enum class WordOp { move, compare, add, subtract };

template <typename Image>
Instruction decode_word_op(const Image& image, Location where, std::uint16_t opcode, WordOp op) {
    unsigned words = 1;
    const auto extension = [&] {
        const auto value = image.word(where, 2 * words);
        ++words;
        return value;
    };
    const auto source = (opcode >> 6) & 077;
    const auto destination = opcode & 077;

    std::optional<std::uint16_t> immediate;
    std::string src;
    if (source <= 6) {
        src = reg(source);
    } else if (source == immediate_pc) {
        immediate = extension();
        src = std::format("0x{:04X}", *immediate);
    } else if constexpr (Image::banked) {
        if (source != absolute_pc) {
            unsupported(where, opcode, true);
        }
        src = read_absolute(image, where, extension());
    } else {
        unsupported(where, opcode, false);
    }

    std::string dst;
    auto next = where.advance(static_cast<int>(2 * words));
    if (destination <= 6) {
        dst = reg(destination);
    } else if constexpr (Image::banked) {
        if (destination != absolute_pc) {
            unsupported(where, opcode, true);
        }
        const auto address = extension();
        next = where.advance(static_cast<int>(2 * words));
        if (const auto window = aot::bank_selector(address); window && op == WordOp::move) {
            if (!immediate) {
                throw std::runtime_error(std::format("bank selector write to 0x{:04X} at {} needs an immediate source",
                                                     address, aot::describe(where, true)));
            }
            // Both extension words are fetched before the selector changes the mapping.
            next.banks[*window] = aot::selected_bank(*immediate);
            return sequential(next, words, std::format("ops::move(s.flags, {})", src));
        }
        dst = op == WordOp::compare ? read_absolute(image, where, address) : ram_operand(where, address, "write");
    } else {
        unsupported(where, opcode, false);
    }

    switch (op) {
    case WordOp::move:
        return sequential(next, words, std::format("{} = ops::move(s.flags, {})", dst, src));
    case WordOp::compare:
        // CMP sets flags for source minus destination and leaves the operands intact.
        return sequential(next, words, std::format("ops::subtract(s.flags, {}, {})", src, dst));
    case WordOp::add:
        return sequential(next, words, std::format("{} = ops::add(s.flags, {}, {})", dst, dst, src));
    case WordOp::subtract:
        return sequential(next, words, std::format("{} = ops::subtract(s.flags, {}, {})", dst, dst, src));
    }
    unsupported(where, opcode, Image::banked);
}

template <typename Image>
Instruction decode(const Image& image, Location where) {
    const auto opcode = image.word(where);
    const auto next = where.advance(2);

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
        return {2, "    " + jump(entry), {entry}};
    }

    switch (opcode & 0170000) {
    case 0010000: return decode_word_op(image, where, opcode, WordOp::move);
    case 0020000: return decode_word_op(image, where, opcode, WordOp::compare);
    case 0060000: return decode_word_op(image, where, opcode, WordOp::add);
    case 0160000: return decode_word_op(image, where, opcode, WordOp::subtract);
    }

    const auto destination = opcode & 077;
    const auto unary = opcode & 0177700;
    if constexpr (Image::banked) {
        if (destination == absolute_pc && (unary == 0005000 || unary == 0105000)) {
            const bool byte = unary == 0105000;
            const auto address = image.word(where, 2);
            const auto dst = ram_operand(where, address, "write", byte);
            const auto operation = byte
                ? std::format("{} = ops::clear_byte(s.flags, {}, {})", dst, dst, address % 2 != 0)
                : std::format("{} = ops::clear(s.flags)", dst);
            return sequential(where.advance(4), 2, operation);
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
    case 0005700: return sequential(next, 1, std::format("ops::test(s.flags, {})", dst));
    }
    unsupported(where, opcode, Image::banked);
}

using Program = std::map<Location, Instruction>;

template <typename Image>
Program analyze(const Image& image, Location entry) {
    Program decoded;
    std::set<Location> operands;
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

            auto instruction = decode(image, where);
            for (unsigned index = 1; index < instruction.words; ++index) {
                const auto operand = where.advance(static_cast<int>(2 * index));
                if (decoded.contains(operand)) {
                    throw overlap(operand);
                }
                operands.insert(operand);
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
               "    [[maybe_unused]] auto& s = board.cpu;\n";
    } else {
        out << "Stop run([[maybe_unused]] State& s, std::uint64_t budget) {\n";
    }
    out << "    " << jump(entry);
    for (const auto& [where, instruction] : decoded) {
        out << label(where) << ":\n"
            << "    if (budget == 0) " << stop("budget", where)
            << "    --budget;\n" << instruction.body;
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
    if (argc != (inspect ? 5 : banked ? 6 : 4)) {
        std::cerr << "Usage: apb-t11-aot <image.bin> <hex-origin> <output.cpp>\n"
                  << "       apb-t11-aot --system2 <rom-map> <rom-dir> <hex-entry> <output.cpp>\n"
                  << "       apb-t11-aot --inspect-system2 <rom-map> <rom-dir> <hex-entry>\n";
        return 2;
    }
    try {
        if (banked) {
            const Location entry{aot::parse_address(argv[4])};
            const std::filesystem::path output = inspect ? std::filesystem::path{} : argv[5];
            const aot::System2Image image(argv[2], argv[3], output);
            if (inspect) {
                std::cout << "Inspecting T-11 entry " << aot::describe(entry, true) << ".\n" << std::flush;
            }
            const auto decoded = analyze(image, entry);
            if (!inspect) {
                write_program(output, entry, decoded, banked);
            }
            std::cout << (inspect ? "Analyzed " : "Translated ") << decoded.size() << " reachable T-11 locations.\n";
        } else {
            const Location entry{aot::parse_address(argv[2])};
            const std::filesystem::path output = argv[3];
            const aot::FlatImage image(argv[1], entry.pc, output);
            const auto decoded = analyze(image, entry);
            write_program(output, entry, decoded, banked);
            std::cout << "Translated " << decoded.size() << " reachable T-11 instructions.\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "apb-t11-aot: " << error.what() << '\n';
        return 1;
    }
}
