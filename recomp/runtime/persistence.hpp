#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace qilin::persist {
// A unique sibling temporary file, flushed to disk and atomically renamed.
// Failure before replacement leaves the previous destination intact.
bool replace(const std::string& path, std::span<const std::uint8_t> bytes, std::string* error = nullptr);
inline bool replace(const std::string& path, std::string_view text, std::string* error = nullptr) {
    return replace(path, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, error);
}

// Brings an older data folder's files into the current one: each file the
// current folder doesn't have is copied across, and only once every copy
// has succeeded are the old ones removed (and the old folder, if that
// leaves it empty). Nothing is overwritten. False if a file couldn't be
// copied: then nothing has changed and the old folder is still complete.
// *moved gets how many files came across. The same folder by another name
// (a file system that ignores case) is left as it is.
bool adopt_folder(const std::string& from, const std::string& to, int* moved = nullptr, std::string* error = nullptr);

class Eeprom {
public:
    using Bytes = std::array<std::uint8_t, 512>;
    enum class Loaded { fresh, primary, backup, invalid };
    explicit Eeprom(std::string path) : path_(std::move(path)) {}
    Loaded load(Bytes& bytes);
    // At most one write attempt per five seconds, and only if changed.
    bool poll(const Bytes& bytes, std::uint64_t milliseconds);
    bool save(const Bytes& bytes);
    [[nodiscard]] const std::string& error() const { return error_; }
private:
    std::string path_, error_;
    Bytes saved_{};
    bool have_saved_ = false;
    bool repair_ = false;
    std::uint64_t last_attempt_ = 0;
};
} // namespace qilin::persist
