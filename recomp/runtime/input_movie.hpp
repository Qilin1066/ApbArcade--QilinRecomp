#pragma once
#include "system2_machine.hpp"
#include <cstdio>
#include <string>
#include <vector>

namespace qilin::movie {
class Reader {
public:
    bool load(const std::string& path);
    void apply(std::uint64_t frame, sys2::Inputs& input) const;
    [[nodiscard]] std::uint64_t duration() const { return duration_; }
    [[nodiscard]] bool interrupted() const { return interrupted_; }
    [[nodiscard]] const std::array<std::uint8_t, 512>& eeprom() const { return eeprom_; }
    [[nodiscard]] const std::string& version() const { return version_; }
    [[nodiscard]] const std::string& error() const { return error_; }
private:
    std::array<std::uint8_t, 512> eeprom_{};
    std::vector<sys2::Inputs> inputs_;
    std::uint64_t duration_ = 0;
    bool interrupted_ = false;
    std::string version_, error_;
};
class Writer {
public:
    ~Writer();
    bool open(const std::string& path, const std::array<std::uint8_t, 512>& eeprom, const sys2::Inputs& initial);
    bool frame(std::uint64_t number, const sys2::Inputs& input);
    bool finish(std::uint64_t duration);
    [[nodiscard]] bool active() const { return file_ != nullptr; }
    [[nodiscard]] const std::string& error() const { return error_; }
private:
    bool flush();
    bool failed(const std::string& error);
    std::FILE* file_ = nullptr;
    std::uint64_t next_ = 1;
    std::vector<std::uint8_t> buffer_;
    std::string error_;
};
} // namespace qilin::movie
