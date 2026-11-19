#pragma once
// printf-style formatting into a std::string, for the runtime: Apple's libc++
// has std::format only from macOS 13.3, and the runtime runs on 10.15.

#include <cstdarg>
#include <cstdio>
#include <string>

namespace qilin::app {

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline std::string strf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list again;
    va_copy(again, args);
    const int n = std::vsnprintf(nullptr, 0, format, args);
    va_end(args);
    std::string out(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), out.size() + 1, format, again);
    va_end(again);
    return out;
}

}  // namespace qilin::app
