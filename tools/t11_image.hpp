#pragma once

#include <qilin/t11.hpp>

#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::t11::aot {

using Address = std::uint16_t;
using JumpTables = std::map<Address, unsigned>;

struct Location {
    Address pc;
    Banks banks{};

    auto operator<=>(const Location&) const = default;

    [[nodiscard]] Location at(Address address) const {
        return {address, banks};
    }

    [[nodiscard]] Location advance(int bytes) const {
        return at(static_cast<Address>(pc + bytes));
    }
};

[[nodiscard]] std::string describe(Location where, bool banked);
[[nodiscard]] Address parse_address(std::string_view text);
[[nodiscard]] JumpTables read_jump_tables(const std::filesystem::path& path, const std::filesystem::path& output);

class FlatImage {
public:
    static constexpr bool banked = false;

    FlatImage(const std::filesystem::path& path, Address origin, const std::filesystem::path& output);
    [[nodiscard]] std::uint16_t word(Location where, unsigned offset = 0) const;

private:
    Address origin_;
    std::vector<std::uint8_t> bytes_;
};

class System2Image {
public:
    static constexpr bool banked = true;

    System2Image(const std::filesystem::path& layout, const std::filesystem::path& rom_dir,
                 const std::filesystem::path& output);
    [[nodiscard]] std::uint16_t word(Location where, unsigned offset = 0) const;
    [[nodiscard]] std::optional<std::uint16_t> rom_data_word(Location where, Address address) const;

private:
    std::vector<std::optional<std::uint16_t>> words_;
};

[[nodiscard]] std::optional<unsigned> bank_selector(Address address);

} // namespace qilin::t11::aot
