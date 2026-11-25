#include "input_movie.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>

namespace qilin::movie {
namespace {
constexpr std::uint64_t max_frames = 2160000; // ten hours; bounded memory for playback
constexpr std::size_t header_size = 8 + 32 + 512 + 13 + 4;
std::uint32_t crc(std::span<const std::uint8_t> data) {
    std::uint32_t value = 0xffffffffu;
    for (const auto byte : data) {
        value ^= byte;
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1)));
    }
    return ~value;
}
void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, int count) {
    for (int i = 0; i < count; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
std::uint64_t get(std::span<const std::uint8_t> bytes, std::size_t& at, int count) {
    std::uint64_t value = 0;
    for (int i = 0; i < count; ++i) value |= std::uint64_t{bytes[at++]} << (i * 8);
    return value;
}
void encode(std::vector<std::uint8_t>& bytes, const sys2::Inputs& in) {
    put(bytes, in.in0, 2); put(bytes, in.in1, 1); put(bytes, in.dsw0, 1); put(bytes, in.dsw1, 1);
    for (const auto v : in.adc) put(bytes, v, 1);
    for (const auto v : in.leta) put(bytes, v, 1);
}
sys2::Inputs decode(std::span<const std::uint8_t> bytes, std::size_t& at) {
    sys2::Inputs in;
    in.in0 = static_cast<std::uint16_t>(get(bytes, at, 2));
    in.in1 = static_cast<std::uint8_t>(get(bytes, at, 1));
    in.dsw0 = static_cast<std::uint8_t>(get(bytes, at, 1));
    in.dsw1 = static_cast<std::uint8_t>(get(bytes, at, 1));
    for (auto& v : in.adc) v = static_cast<std::uint8_t>(get(bytes, at, 1));
    for (auto& v : in.leta) v = static_cast<std::uint8_t>(get(bytes, at, 1));
    return in;
}
bool valid(std::span<const std::uint8_t> bytes) {
    std::size_t at = bytes.size() - 4;
    return crc(bytes.first(at)) == get(bytes, at, 4);
}
std::filesystem::path native(const std::string& path) { return std::filesystem::path(std::u8string(path.begin(), path.end())); }
}
Writer::~Writer() { if (file_) std::fclose(file_); }
bool Writer::failed(const std::string& message) {
    error_ = message;
    if (file_) std::fclose(file_);
    file_ = nullptr;
    return false;
}
bool Writer::flush() {
    put(buffer_, crc(buffer_), 4);
    if (std::fwrite(buffer_.data(), 1, buffer_.size(), file_) != buffer_.size() || std::fflush(file_) != 0)
        return failed("Input recording write failed");
    return true;
}
bool Writer::open(const std::string& path, const std::array<std::uint8_t, 512>& eeprom, const sys2::Inputs& initial) {
    if (file_) return false;
#ifdef _WIN32
    file_ = _wfopen(native(path).c_str(), L"wbx");
#else
    file_ = std::fopen(native(path).c_str(), "wbx");
#endif
    if (!file_) { error_ = "Cannot create input recording: " + std::string(std::strerror(errno)); return false; }
    error_.clear(); next_ = 1;
    buffer_.assign({'A','P','B','M','O','V','1',0});
    std::array<char, 32> version{};
    const std::string build = QILIN_MOVIE_VERSION;
    std::copy_n(build.data(), std::min(build.size(), version.size() - 1), version.data());
    for (const auto v : version) buffer_.push_back(static_cast<std::uint8_t>(v));
    buffer_.insert(buffer_.end(), eeprom.begin(), eeprom.end());
    encode(buffer_, initial);
    return flush();
}
bool Writer::frame(std::uint64_t number, const sys2::Inputs& input) {
    if (!file_) return false;
    if (number != next_ || number >= max_frames) return failed("Input recording frame order or duration is invalid");
    buffer_.assign(1, 'F'); put(buffer_, number, 8); encode(buffer_, input);
    if (!flush()) return false;
    ++next_;
    return true;
}
bool Writer::finish(std::uint64_t duration) {
    if (!file_) return false;
    if (duration > next_ || duration + 1 < next_) return failed("Invalid recording end frame");
    buffer_.assign(1, 'E'); put(buffer_, duration, 8);
    if (!flush()) return false;
    const int closed = std::fclose(file_); file_ = nullptr;
    if (closed) { error_ = "Input recording could not close"; return false; }
    return true;
}
bool Reader::load(const std::string& path) {
    inputs_.clear(); error_.clear(); duration_ = 0; interrupted_ = false;
    auto fail = [&](const std::string& message) { error_ = message; inputs_.clear(); return false; };
    std::ifstream input(native(path), std::ios::binary);
    std::vector<std::uint8_t> header(header_size);
    if (!input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size())) ||
        std::memcmp(header.data(), "APBMOV1\0", 8) || !valid(header)) return fail("Invalid or truncated input recording header");
    const auto zero = std::find(header.begin() + 8, header.begin() + 40, 0);
    version_.assign(header.begin() + 8, zero);
    std::copy_n(header.begin() + 40, 512, eeprom_.begin());
    std::size_t at = 552;
    inputs_.push_back(decode(header, at));
    std::vector<std::uint8_t> record;   // reused: one allocation, not one a frame
    for (;;) {
        const int type = input.get();
        if (type == EOF) break;
        if (type != 'F' && type != 'E') return fail("Invalid input recording record");
        record.assign(type == 'F' ? 26 : 13, 0);
        record[0] = static_cast<std::uint8_t>(type);
        if (!input.read(reinterpret_cast<char*>(record.data() + 1), static_cast<std::streamsize>(record.size() - 1))) break;
        if (!valid(record)) return fail("Input recording checksum failed");
        at = 1;
        const auto frame_number = get(record, at, 8);
        if (type == 'E') {
            if (frame_number > inputs_.size() || frame_number + 1 < inputs_.size() || input.peek() != EOF)
                return fail("Invalid input recording end frame");
            duration_ = frame_number;
            return duration_ != 0 || fail("Recording ended before any game frames");
        }
        if (frame_number != inputs_.size() || inputs_.size() >= max_frames) return fail("Input recording frames are unordered or too long");
        inputs_.push_back(decode(record, at));
    }
    if (!input.eof()) return fail("Input recording read failed");
    interrupted_ = true;
    duration_ = inputs_.size();  // replay only the complete, CRC-validated prefix
    return true;
}
void Reader::apply(std::uint64_t frame, sys2::Inputs& input) const {
    if (frame < inputs_.size()) input = inputs_[static_cast<std::size_t>(frame)];
}
} // namespace qilin::movie
