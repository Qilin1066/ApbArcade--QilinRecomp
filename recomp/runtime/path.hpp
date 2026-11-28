#pragma once
// UTF-8/native filesystem conversion, with no platform API headers (Toy
// Commander's). Every path the programs take or show is UTF-8 text; a narrow
// std::filesystem::path or std::string path on Windows means the ANSI code page.
#include <filesystem>
#include <string>
#include <string_view>

namespace qilin {
[[nodiscard]] inline std::filesystem::path native_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
[[nodiscard]] inline std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace qilin
