#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using Address = std::uint16_t;
constexpr std::uint32_t address_space_size = 0x10000;

struct Image {
    Address origin;
    std::span<const std::uint8_t> bytes;

    [[nodiscard]] std::uint16_t word(std::uint32_t address) const {
        if (address < origin || address - origin + 2 > bytes.size() || address % 2 != 0) {
            throw std::runtime_error(std::format("word at 0x{:04X} is outside the input image", address));
        }

        const auto offset = address - origin;
        return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
    }
};

struct Instruction {
    unsigned words = 1;
    std::string body;
    std::vector<Address> successors;
};

std::string label(Address address) {
    return std::format("pc_{:04X}", address);
}

std::string jump(Address address) {
    return std::format("goto {};\n", label(address));
}

std::string reg(unsigned number) {
    return std::format("s.r[{}]", number);
}

[[noreturn]] void unsupported(Address pc, std::uint16_t opcode) {
    throw std::runtime_error(std::format("unsupported T-11 instruction 0x{:04X} at 0x{:04X}", opcode, pc));
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

Instruction branch(Address next, Address target, std::string_view condition) {
    return {
        1,
        std::format("    if ({}) {}    {}", condition, jump(target), jump(next)),
        {target, next}
    };
}

Instruction decode(const Image& image, Address pc) {
    const auto opcode = image.word(pc);
    const auto next = static_cast<Address>(pc + 2);

    if (opcode == 0000000) {
        return {1, std::format("    return {{StopReason::halt, 0x{:04X}}};\n", pc), {}};
    }
    if (opcode == 0000240) {
        return {1, "    " + jump(next), {next}};
    }

    const auto displacement = static_cast<std::int8_t>(opcode & 0xff);
    const auto target = static_cast<Address>(next + 2 * displacement);
    if ((opcode & 0177400) == 0000400) {
        return {1, "    " + jump(target), {target}};
    }
    if (const auto condition = branch_condition(opcode); !condition.empty()) {
        return branch(next, target, condition);
    }
    if ((opcode & 0177000) == 0077000) {
        const auto counter = (opcode >> 6) & 7;
        if (counter == 7) {
            unsupported(pc, opcode);
        }
        const auto loop = static_cast<Address>(next - 2 * (opcode & 077));
        return branch(next, loop, "--" + reg(counter) + " != 0");
    }

    const auto destination = opcode & 077;
    if (destination > 6) {
        unsupported(pc, opcode);
    }
    const auto dst = reg(destination);
    std::string operation;
    switch (opcode & 0177700) {
    case 0005000: operation = std::format("{} = ops::clear(s.flags)", dst); break;
    case 0005200: operation = std::format("{} = ops::increment(s.flags, {})", dst, dst); break;
    case 0005300: operation = std::format("{} = ops::decrement(s.flags, {})", dst, dst); break;
    case 0005700: operation = std::format("ops::test(s.flags, {})", dst); break;
    }
    if (!operation.empty()) {
        return {1, std::format("    {};\n    {}", operation, jump(next)), {next}};
    }

    const auto source = (opcode >> 6) & 077;
    constexpr unsigned immediate_pc = 027;
    if (source > 6 && source != immediate_pc) {
        unsupported(pc, opcode);
    }
    switch (opcode & 0170000) {
    case 0010000:
    case 0020000:
    case 0060000:
    case 0160000:
        break;
    default:
        unsupported(pc, opcode);
    }

    const bool immediate = source == immediate_pc;
    const auto src = immediate ? std::format("0x{:04X}", image.word(std::uint32_t{pc} + 2))
                               : reg(source);
    switch (opcode & 0170000) {
    case 0010000: operation = std::format("{} = ops::move(s.flags, {})", dst, src); break;
    // CMP sets flags for source minus destination and leaves both registers intact.
    case 0020000: operation = std::format("ops::subtract(s.flags, {}, {})", src, dst); break;
    case 0060000: operation = std::format("{} = ops::add(s.flags, {}, {})", dst, dst, src); break;
    case 0160000: operation = std::format("{} = ops::subtract(s.flags, {}, {})", dst, dst, src); break;
    }
    const auto successor = immediate ? static_cast<Address>(pc + 4) : next;
    return {immediate ? 2u : 1u, std::format("    {};\n    {}", operation, jump(successor)), {successor}};
}

std::map<Address, Instruction> analyze(const Image& image) {
    std::map<Address, Instruction> decoded;
    std::set<Address> operands;
    std::vector<Address> pending{image.origin};

    while (!pending.empty()) {
        const auto pc = pending.back();
        pending.pop_back();
        if (decoded.contains(pc)) {
            continue;
        }
        if (operands.contains(pc)) {
            throw std::runtime_error(std::format("branch enters operand word at 0x{:04X}", pc));
        }

        auto instruction = decode(image, pc);
        if (instruction.words == 2) {
            // decode() already checked that this word fits inside the image.
            const auto operand = static_cast<Address>(pc + 2);
            if (decoded.contains(operand)) {
                throw std::runtime_error(std::format("branch enters operand word at 0x{:04X}", operand));
            }
            operands.insert(operand);
        }
        pending.insert(pending.end(), instruction.successors.begin(), instruction.successors.end());
        decoded.emplace(pc, std::move(instruction));
    }
    return decoded;
}

void emit(std::ostream& out, Address entry, const std::map<Address, Instruction>& decoded) {
    out << "#include <qilin/t11.hpp>\n\n"
           "namespace qilin::t11 {\n\n"
           "Stop run([[maybe_unused]] State& s, std::uint64_t budget) {\n"
        << "    " << jump(entry);
    for (const auto& [pc, instruction] : decoded) {
        out << std::format("{}:\n"
                           "    if (budget == 0) return {{StopReason::budget, 0x{:04X}}};\n"
                           "    --budget;\n", label(pc), pc)
            << instruction.body;
    }
    out << "}\n\n} // namespace qilin::t11\n";
}

Address parse_origin(std::string_view text) {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    Address origin{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), origin, 16);
    if (error != std::errc{} || end != text.data() + text.size() || origin % 2 != 0) {
        throw std::runtime_error("origin must be an even 16-bit hexadecimal address");
    }
    return origin;
}

std::vector<std::uint8_t> read_image(const std::filesystem::path& path, Address origin) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("cannot open input image");
    }

    const auto size = static_cast<std::streamoff>(in.tellg());
    if (size < 2 || size % 2 != 0 || size > address_space_size - origin) {
        throw std::runtime_error("input must contain whole words and fit at the specified origin");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("cannot read input image");
    }
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << APB_VERSION << '\n';
        return 0;
    }
    if (argc != 4) {
        std::cerr << "Usage: apb-t11-aot <image.bin> <hex-origin> <output.cpp>\n";
        return 2;
    }
    try {
        const std::filesystem::path input = argv[1];
        const std::filesystem::path output = argv[3];
        if (std::filesystem::exists(output) && std::filesystem::equivalent(input, output)) {
            throw std::runtime_error("input and output must be different files");
        }

        const auto origin = parse_origin(argv[2]);
        const auto bytes = read_image(input, origin);
        const auto decoded = analyze({origin, bytes});

        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open C++ output");
        }
        emit(out, origin, decoded);
        out.close();
        if (!out) {
            throw std::runtime_error("cannot write C++ output");
        }
        std::cout << "Translated " << decoded.size() << " reachable T-11 instructions.\n";
    } catch (const std::exception& error) {
        std::cerr << "apb-t11-aot: " << error.what() << '\n';
        return 1;
    }
}
