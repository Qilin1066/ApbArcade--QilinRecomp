#pragma once
// `qilin identify <rom-dir> --json`: what the player's ROM folder is, for the
// launcher (tools/player-kit/launcher), as the contract every Qilin
// recompiler keeps (ADOPTING.md there). One JSON object on standard output,
//
//   {"ok": true|false, "title": "...", "id": "...", "version": "...", "message": "..."}
//
// and the exit code: 0 for the rev 7 set (MAME's apb: every one of its 36
// files, by size and SHA-1, as `qilin verify` checks them), 3 for a folder
// that can be read but isn't that set (another revision or language of APB,
// told apart by the program ROMs only it has, or no APB files at all), and 2
// when it can't be used (not a folder, missing, empty, a zip still to be
// unpacked, files missing or damaged). The message is a sentence for the
// player, ending with a full stop. Nothing is ever written.

#include "romset.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace qilin::rom {

// A ROM file of another set, by MAME's name, size and SHA-1.
struct Known {
    std::string_view name;
    std::uint32_t size;
    std::string_view sha1;
};

// Another set of APB (an earlier revision, the German or French version),
// as MAME has it (atarisy2.cpp): only what differs from rev 7. `files` are
// its own program ROMs, `instead` the rev 7 files it doesn't have.
struct OtherSet {
    std::string_view set;      // MAME's name: apb6
    std::string_view title;    // MAME's description: APB - All Points Bulletin (rev 6)
    std::string_view version;  // 6, German
    std::span<const Known> files;
    std::span<const std::string_view> instead;
};

[[nodiscard]] std::span<const OtherSet> other_sets() noexcept;

// What the supported set is called.
inline constexpr std::string_view rev7_set = "apb";
inline constexpr std::string_view rev7_title = "APB - All Points Bulletin (rev 7)";
inline constexpr std::string_view rev7_version = "7";

struct Identity {
    int code = 2;  // 0, 3 or 2, as above
    bool ok = false;
    std::string title, id, version, message;
};

// `files` is the supported set and `others` the sets told apart from it
// (the tests give their own).
[[nodiscard]] Identity identify(const std::filesystem::path& input, std::span<const File> files, std::span<const OtherSet> others);
[[nodiscard]] inline Identity identify(const std::filesystem::path& input) {
    return identify(input, rev7_files(), other_sets());
}

// The identity as the contract's one line of JSON (with its newline).
[[nodiscard]] std::string to_json(const Identity& identity);

} // namespace qilin::rom
