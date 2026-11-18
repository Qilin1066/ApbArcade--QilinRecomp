#include "t11_image.hpp"

#include <charconv>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace qilin::t11::aot {
namespace {

constexpr std::uint32_t address_space_size = 0x10000;
constexpr std::uint32_t fixed_rom_start = 0x8000;
constexpr std::uint32_t slapstic_end = 0x8200;
constexpr std::uint32_t window_start = 0x4000;
constexpr std::uint32_t window_size = 0x2000;
constexpr std::uint32_t bank_rom_start = 0x10000;
constexpr std::uint32_t physical_rom_size = bank_rom_start + 64 * window_size;

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

std::optional<std::uint32_t> parse_hex(std::string_view text) {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path, std::uint32_t max_size,
                                     const std::filesystem::path& output) {
    if (!output.empty() && std::filesystem::exists(output) && std::filesystem::equivalent(path, output)) {
        throw std::runtime_error("input and output must be different files");
    }

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("cannot open input image " + path.string());
    }

    const auto size = static_cast<std::streamoff>(in.tellg());
    if (size < 1 || size > max_size) {
        throw std::runtime_error("input image is empty or too large: " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("cannot read input image " + path.string());
    }
    return bytes;
}

bool is_filename(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string_view::npos;
}

} // namespace

std::string describe(Location where, bool banked) {
    if (banked) {
        return std::format("0x{:04X} (banks {:02X}/{:02X})", where.pc, where.banks[0], where.banks[1]);
    }
    return std::format("0x{:04X}", where.pc);
}

Address parse_address(std::string_view text) {
    const auto address = parse_hex(text);
    if (!address || *address >= address_space_size || *address % 2 != 0) {
        throw std::runtime_error("origin must be an even 16-bit hexadecimal address");
    }
    return static_cast<Address>(*address);
}

FlatImage::FlatImage(const std::filesystem::path& path, Address origin, const std::filesystem::path& output)
    : origin_(origin), bytes_(read_bytes(path, address_space_size - origin, output)) {
    if (bytes_.size() % 2 != 0) {
        throw std::runtime_error("input must contain whole words and fit at the specified origin");
    }
}

std::uint16_t FlatImage::word(Location where, unsigned offset) const {
    const auto address = static_cast<std::uint32_t>(where.pc) + offset;
    if (address < origin_ || address - origin_ + 2 > bytes_.size() || address % 2 != 0) {
        throw std::runtime_error(std::format("word at 0x{:04X} is outside the input image", address));
    }
    const auto index = address - origin_;
    return static_cast<std::uint16_t>(bytes_[index] | (bytes_[index + 1] << 8));
}

System2Image::System2Image(const std::filesystem::path& layout, const std::filesystem::path& rom_dir,
                           const std::filesystem::path& output)
    : words_(physical_rom_size / 2) {
    if (!output.empty() && std::filesystem::exists(output) && std::filesystem::equivalent(layout, output)) {
        throw std::runtime_error("input and output must be different files");
    }

    std::ifstream in(layout);
    if (!in) {
        throw std::runtime_error("cannot open System 2 ROM map " + layout.string());
    }

    unsigned line_number = 0;
    bool loaded_pair = false;
    for (std::string storage; std::getline(in, storage);) {
        ++line_number;
        const auto line = trim(storage);
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const auto first = line.find('|');
        const auto second = first == std::string_view::npos ? first : line.find('|', first + 1);
        if (second == std::string_view::npos || line.find('|', second + 1) != std::string_view::npos) {
            throw std::runtime_error(std::format("ROM map line {} must contain offset|even-file|odd-file", line_number));
        }

        const auto base = parse_hex(trim(line.substr(0, first)));
        const auto even_name = trim(line.substr(first + 1, second - first - 1));
        const auto odd_name = trim(line.substr(second + 1));
        if (!base || *base < fixed_rom_start || *base >= physical_rom_size || *base % 2 != 0 ||
            !is_filename(even_name) || !is_filename(odd_name)) {
            throw std::runtime_error(std::format("invalid offset or file name in ROM map line {}", line_number));
        }

        const auto max_bytes = (physical_rom_size - *base) / 2;
        const auto even = read_bytes(rom_dir / even_name, max_bytes, output);
        const auto odd = read_bytes(rom_dir / odd_name, max_bytes, output);
        if (even.size() != odd.size()) {
            throw std::runtime_error(std::format("ROM map line {} has mismatched byte lane sizes", line_number));
        }

        for (std::size_t index = 0; index < even.size(); ++index) {
            auto& word = words_[*base / 2 + index];
            if (word) {
                throw std::runtime_error(std::format("overlapping ROM pairs at physical offset 0x{:05X}",
                                                      *base + 2 * index));
            }
            word = static_cast<std::uint16_t>(even[index] | (odd[index] << 8));
        }
        loaded_pair = true;
    }
    if (!in.eof()) {
        throw std::runtime_error("cannot read System 2 ROM map " + layout.string());
    }
    if (!loaded_pair) {
        throw std::runtime_error("System 2 ROM map is empty");
    }
}

std::uint16_t System2Image::word(Location where, unsigned offset) const {
    const auto address = static_cast<Address>(where.pc + offset);
    if (address < window_start || address % 2 != 0) {
        throw std::runtime_error("word at " + describe(where.at(address), banked) + " is outside aligned System 2 ROM");
    }

    std::uint32_t physical = address;
    if (address < fixed_rom_start) {
        const auto window = (address - window_start) / window_size;
        physical = bank_rom_start + where.banks[window] * window_size + (address % window_size);
    }

    const auto word = words_[physical / 2];
    if (!word) {
        throw std::runtime_error(std::format("missing ROM at {} (physical offset 0x{:05X})",
                                             describe(where.at(address), banked), physical));
    }
    return *word;
}

std::optional<std::uint16_t> System2Image::rom_data_word(Location where, Address address) const {
    if (address < window_start || (address >= fixed_rom_start && address < slapstic_end)) {
        return std::nullopt;
    }
    return word(where.at(address));
}

std::optional<unsigned> bank_selector(Address address) {
    // The two 16-bit registers are mirrored through 0x147F.
    if (address < 0x1400 || address >= 0x1480 || address % 2 != 0) {
        return std::nullopt;
    }
    return (address & 2) / 2;
}

std::uint8_t selected_bank(std::uint16_t word) {
    // Two low selector bits are inverted and the two low bit pairs are exchanged.
    const auto selector = static_cast<unsigned>(word >> 10) ^ 3u;
    return static_cast<std::uint8_t>((selector & 0x30) | ((selector & 0x03) << 2) | ((selector & 0x0c) >> 2));
}

} // namespace qilin::t11::aot
