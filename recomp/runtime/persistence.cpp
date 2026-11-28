#include "persistence.hpp"
#include "path.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace qilin::persist {
namespace {
bool read_eeprom(const std::string& path, Eeprom::Bytes& bytes) {
    std::ifstream in(native_path(path), std::ios::binary | std::ios::ate);
    if (!in || in.tellg() != static_cast<std::streamoff>(bytes.size())) return false;
    in.seekg(0);
    Eeprom::Bytes candidate;
    if (!in.read(reinterpret_cast<char*>(candidate.data()), static_cast<std::streamsize>(candidate.size()))) return false;
    bytes = candidate;   // never partially overwrite the board's factory state
    return true;
}
}

bool adopt_folder(const std::string& from, const std::string& to, int* moved, std::string* error) {
    namespace fs = std::filesystem;
    if (moved) *moved = 0;
    if (error) error->clear();
    const fs::path old = native_path(from), current = native_path(to);
    std::error_code ec;
    const bool old_directory = fs::is_directory(old, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        if (error) *error = "Could not inspect " + from + ": " + ec.message();
        return false;
    }
    if (!old_directory) return true;
    ec.clear();
    fs::create_directories(current, ec);
    if (ec) {
        if (error) *error = "Could not create " + to + ": " + ec.message();
        return false;
    }
    if (fs::equivalent(old, current, ec)) return true;
    ec.clear();
    std::vector<fs::path> files, copied, originals;
    for (fs::directory_iterator it(old, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    if (ec) {
        if (error) *error = "Could not read " + from + ": " + ec.message();
        return false;
    }
    for (const auto& file : files) {
        const fs::path target = current / file.filename();
        if (fs::exists(target, ec)) continue;   // the current folder's own wins
        ec.clear();
        // Copied as saves are written (flushed, then renamed into place), so a
        // power cut can't leave a whole-size empty eeprom.bin with the original
        // already gone: the original is removed only after every copy is complete.
        std::ifstream in(file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto name = target.u8string();
        std::string why;
        if (!in.good() && !in.eof()) why = "could not read it";
        else if (!replace(std::string(name.begin(), name.end()), bytes, &why) && why.empty()) why = "could not write it";
        if (!why.empty()) {
            if (error) *error = "Could not copy " + from + " to " + to + ": " + why;
            for (const auto& made : copied) fs::remove(made, ec);
            return false;
        }
        copied.push_back(target);
        originals.push_back(file);
    }
    // Remove only originals copied by this migration. A name collision may
    // contain different scores/settings and must remain available for recovery.
    for (const auto& file : originals) fs::remove(file, ec);
    ec.clear();
    if (fs::is_empty(old, ec) && !ec) {
        fs::remove(old, ec);
        ec.clear();
        if (old.has_parent_path() && fs::is_empty(old.parent_path(), ec) && !ec) fs::remove(old.parent_path(), ec);
    }
    if (moved) *moved = static_cast<int>(copied.size());
    return true;
}

bool replace(const std::string& path, std::span<const std::uint8_t> bytes, std::string* error) {
    if (error) error->clear();
    static std::atomic<unsigned> serial{0};
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto destination = native_path(path);
    std::filesystem::path temporary;
    std::FILE* file = nullptr;
    for (int attempt = 0; attempt < 32 && !file; ++attempt) {
        temporary = native_path(path + ".tmp." + std::to_string(pid) + "." + std::to_string(serial.fetch_add(1)));
#ifdef _WIN32
        file = _wfopen(temporary.c_str(), L"wbx");
#else
        file = std::fopen(temporary.c_str(), "wbx");
#endif
        if (!file && errno != EEXIST) break;
    }
    if (!file) { if (error) *error = "Cannot create save file: " + std::string(std::strerror(errno)); return false; }
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && std::fflush(file) == 0;
#ifdef _WIN32
    if (ok) ok = _commit(_fileno(file)) == 0;
#else
    if (ok) ok = fsync(fileno(file)) == 0;
#endif
    if (std::fclose(file) != 0) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::error_code ec;
        std::filesystem::rename(temporary, destination, ec);
        ok = !ec;
        if (ok) {
            const auto parent = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
            const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
            if (directory >= 0) { (void)fsync(directory); close(directory); }
        }
#endif
    }
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (error) *error = "Could not complete save replacement: " + path;
    }
    return ok;
}

Eeprom::Loaded Eeprom::load(Bytes& bytes) {
    error_.clear();
    have_saved_ = repair_ = false;
    writable_ = true;
    last_attempt_ = 0;
    Loaded result = Loaded::primary;
    if (!read_eeprom(path_, bytes)) {
        if (read_eeprom(path_ + ".bak", bytes)) { result = Loaded::backup; repair_ = true; }
        else {
            std::error_code ec;
            const bool primary = std::filesystem::exists(native_path(path_), ec);
            const bool primary_error = static_cast<bool>(ec);
            ec.clear();
            const bool backup = std::filesystem::exists(native_path(path_ + ".bak"), ec);
            const bool exists = primary || backup || primary_error || static_cast<bool>(ec);
            writable_ = !exists;
            if (exists) error_ = "EEPROM files are incomplete or unreadable; saves disabled to preserve them. Repair or move them aside, then restart";
            return exists ? Loaded::invalid : Loaded::fresh;
        }
    }
    saved_ = bytes;
    have_saved_ = true;
    return result;
}

bool Eeprom::poll(const Bytes& bytes, std::uint64_t milliseconds) {
    if (milliseconds - last_attempt_ < 5000) return true;
    last_attempt_ = milliseconds;
    return save(bytes);
}

bool Eeprom::save(const Bytes& bytes) {
    if (!writable_) return false;
    if (have_saved_ && bytes == saved_ && !repair_) return true;
    // Back up only a complete primary. A damaged primary must never
    // overwrite the backup we just recovered from.
    Bytes previous;
    if (read_eeprom(path_, previous) && !replace(path_ + ".bak", previous, &error_)) return false;
    if (!replace(path_, bytes, &error_)) return false;
    saved_ = bytes;
    have_saved_ = true;
    repair_ = false;
    return true;
}
} // namespace qilin::persist
