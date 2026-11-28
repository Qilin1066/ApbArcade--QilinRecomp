#pragma once
// The APB rev 7 ROM set, loaded into MAME's memory regions.
//
// The file list, sizes, SHA-1 digests and load layout are MAME's
// (src/mame/atari/atarisy2.cpp, ROM_START( apb )). Only the owner's own files
// are read; nothing from them is ever written into the repository.

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::rom {

// Paths given as UTF-8 text (the command line, apb.ini) and back, the same on
// every system: a narrow std::filesystem::path on Windows means the ANSI code page.
[[nodiscard]] inline std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
[[nodiscard]] inline std::string utf8_text(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

enum class Region : std::uint8_t { main_cpu, sound_cpu, tiles, sprites, chars };
inline constexpr std::size_t region_count = 5;

struct RegionInfo {
    std::string_view name;   // MAME's tag
    std::uint32_t size;
    bool inverted;           // ROMREGION_INVERT: every byte complemented after loading
};

[[nodiscard]] const RegionInfo& region_info(Region region) noexcept;

// One ROM file and where MAME puts it. A file is loaded in up to two pieces
// (ROM_LOAD then ROM_CONTINUE). ROM_LOAD16_BYTE files fill every other byte.
struct File {
    struct Piece {
        std::uint32_t offset;  // into the region
        std::uint32_t length;  // bytes of the file
    };
    std::string_view name;
    std::uint32_t size;
    std::string_view sha1;
    Region region;
    bool byte_lane;            // ROM_LOAD16_BYTE
    std::array<Piece, 2> pieces;
};

[[nodiscard]] std::span<const File> rev7_files() noexcept;

// The result of checking one file in a ROM directory.
struct FileCheck {
    const File* file;
    enum class Status { ok, missing, wrong_size, wrong_digest } status;
    std::uint64_t actual_size = 0;
};

// Checks every rev 7 file's size and SHA-1 in 'dir'.
[[nodiscard]] std::vector<FileCheck> verify(const std::filesystem::path& dir);
// The same for any list of files (the tests give their own).
[[nodiscard]] std::vector<FileCheck> verify(const std::filesystem::path& dir, std::span<const File> files);

// The loaded set: each region as MAME builds it, and which bytes a file filled.
class RomSet {
public:
    // Loads and verifies all 36 files; fails with a message naming every
    // problem if any file is missing or doesn't match.
    [[nodiscard]] static std::expected<RomSet, std::string> load(const std::filesystem::path& dir);

    // A set built from given region contents, for tests.
    [[nodiscard]] static RomSet from_regions(std::array<std::vector<std::uint8_t>, region_count> data,
                                             std::array<std::vector<bool>, region_count> populated);

    [[nodiscard]] std::span<const std::uint8_t> region(Region r) const noexcept {
        return data_[static_cast<std::size_t>(r)];
    }
    [[nodiscard]] bool populated(Region r, std::uint32_t offset) const noexcept {
        const auto& p = populated_[static_cast<std::size_t>(r)];
        return offset < p.size() && p[offset];
    }
    // Little-endian word at an even offset of a region, if both bytes come from a file.
    [[nodiscard]] std::optional<std::uint16_t> word(Region r, std::uint32_t offset) const noexcept;

private:
    std::array<std::vector<std::uint8_t>, region_count> data_;
    std::array<std::vector<bool>, region_count> populated_;
};

// SHA-1 of a byte string, as 40 lowercase hex digits.
[[nodiscard]] std::string sha1(std::span<const std::uint8_t> bytes);

} // namespace qilin::rom
