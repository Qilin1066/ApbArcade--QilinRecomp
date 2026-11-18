#pragma once
// The few pieces of MAME's emu.h that MAME's T-11 disassembler (t11dasm.cpp)
// uses, so that file compiles here unmodified for tools/check.

#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <type_traits>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using offs_t = std::uint32_t;

#define BIT(x, n) (((x) >> (n)) & 1)

namespace util {

namespace detail {
template <typename T>
auto printf_arg(const T& value) {
    if constexpr (std::is_same_v<T, std::string>) {
        return value.c_str();
    } else {
        return value;
    }
}
} // namespace detail

template <typename... Args>
std::string string_format(const char* format, const Args&... args) {
    char buffer[256];
    const int length = std::snprintf(buffer, sizeof buffer, format, detail::printf_arg(args)...);
    return {buffer, static_cast<std::size_t>(length > 0 ? length : 0)};
}

template <typename... Args>
void stream_format(std::ostream& stream, const char* format, const Args&... args) {
    stream << string_format(format, args...);
}

class disasm_interface {
public:
    enum : u32 {
        LENGTHMASK = 0x0000ffff,
        STEP_OUT = 0x80000000,
        STEP_OVER = 0x40000000,
        STEP_COND = 0x20000000,
        SUPPORTED = 0x10000000,
    };

    class data_buffer {
    public:
        explicit data_buffer(const u16* words, offs_t base) : words_(words), base_(base) {}
        u16 r16(offs_t pc) const { return words_[((pc - base_) & 0xffff) / 2]; }
    private:
        const u16* words_;
        offs_t base_;
    };

    virtual ~disasm_interface() = default;
    virtual u32 opcode_alignment() const = 0;
    virtual offs_t disassemble(std::ostream& stream, offs_t pc, const data_buffer& opcodes, const data_buffer& params) = 0;
};

} // namespace util
