#pragma once
// MAME 0.264's attotime (src/emu/attotime.h and attotime.cpp, BSD-3-Clause,
// copyright Aaron Giles), for the diagnostic MAME-scheduling mode: whole
// seconds and attoseconds (10^-18 s), with MAME's arithmetic exactly, its
// rounding included (division rounds half up; HZ_TO_ATTOSECONDS truncates).
// Portable (no 128-bit integer type): products that need more than 64 bits
// go through 32-bit halves, as MAME's own code does.

#include <compare>
#include <cstdint>

namespace qilin::mame {

inline constexpr std::int64_t attos_per_second = 1'000'000'000'000'000'000;
inline constexpr std::int64_t attos_per_second_sqrt = 1'000'000'000;
inline constexpr std::int32_t max_seconds = 1'000'000'000;   // ATTOTIME_MAX_SECONDS: 'never' at or above

[[nodiscard]] constexpr std::int64_t hz_to_attoseconds(std::uint32_t hz) noexcept { return attos_per_second / hz; }

struct Time {
    std::int32_t seconds = 0;
    std::int64_t attoseconds = 0;   // 0 .. 10^18 - 1

    static constexpr Time never() noexcept { return {max_seconds, 0}; }
    [[nodiscard]] constexpr bool is_never() const noexcept { return seconds >= max_seconds; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return seconds == 0 && attoseconds == 0; }

    friend constexpr bool operator==(const Time&, const Time&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const Time& a, const Time& b) noexcept {
        if (const auto c = a.seconds <=> b.seconds; c != 0) return c;
        return a.attoseconds <=> b.attoseconds;
    }

    friend constexpr Time operator+(const Time& l, const Time& r) noexcept {
        if (l.seconds >= max_seconds || r.seconds >= max_seconds) return never();
        Time t{l.seconds + r.seconds, l.attoseconds + r.attoseconds};
        if (t.attoseconds >= attos_per_second) {
            t.attoseconds -= attos_per_second;
            ++t.seconds;
        }
        return t.seconds >= max_seconds ? never() : t;
    }
    friend constexpr Time operator-(const Time& l, const Time& r) noexcept {
        if (l.seconds >= max_seconds) return never();
        Time t{l.seconds - r.seconds, l.attoseconds - r.attoseconds};
        if (t.attoseconds < 0) {
            t.attoseconds += attos_per_second;
            --t.seconds;
        }
        return t;
    }
    Time& operator+=(const Time& r) noexcept { return *this = *this + r; }
    Time& operator-=(const Time& r) noexcept { return *this = *this - r; }

    // MAME's as_attoseconds: clamped to +/- 1 second.
    [[nodiscard]] constexpr std::int64_t as_attoseconds() const noexcept {
        return seconds == 0    ? attoseconds
               : seconds == -1 ? attoseconds - attos_per_second
               : seconds > 0   ? attos_per_second
                               : -attos_per_second;
    }
    [[nodiscard]] double as_double() const noexcept {
        return static_cast<double>(seconds) + static_cast<double>(attoseconds) * 1e-18;
    }

    // operator*=(u32): exact.
    [[nodiscard]] constexpr Time times(std::uint32_t factor) const noexcept {
        if (seconds >= max_seconds) return never();
        if (factor == 0) return {};
        const auto attohi = static_cast<std::uint32_t>(attoseconds / attos_per_second_sqrt);
        const auto attolo = static_cast<std::uint32_t>(attoseconds % attos_per_second_sqrt);
        std::uint64_t temp = static_cast<std::uint64_t>(attolo) * factor;
        const auto reslo = static_cast<std::uint32_t>(temp % attos_per_second_sqrt);
        temp /= attos_per_second_sqrt;
        temp += static_cast<std::uint64_t>(attohi) * factor;
        const auto reshi = static_cast<std::uint32_t>(temp % attos_per_second_sqrt);
        temp /= attos_per_second_sqrt;
        temp += static_cast<std::uint64_t>(static_cast<std::uint32_t>(seconds)) * factor;
        if (temp >= static_cast<std::uint64_t>(max_seconds)) return never();
        return {static_cast<std::int32_t>(temp), static_cast<std::int64_t>(reslo) + static_cast<std::int64_t>(reshi) * attos_per_second_sqrt};
    }
    // operator/=(u32): rounds half up, as MAME's.
    [[nodiscard]] constexpr Time divided(std::uint32_t factor) const noexcept {
        if (seconds >= max_seconds) return never();
        if (factor == 0) return *this;
        const auto attohi = static_cast<std::uint32_t>(attoseconds / attos_per_second_sqrt);
        const auto attolo = static_cast<std::uint32_t>(attoseconds % attos_per_second_sqrt);
        const auto secs = static_cast<std::uint64_t>(static_cast<std::uint32_t>(seconds));
        Time t;
        t.seconds = static_cast<std::int32_t>(secs / factor);
        std::uint64_t remainder = secs % factor;
        std::uint64_t temp = attohi + remainder * attos_per_second_sqrt;
        const auto reshi = static_cast<std::uint32_t>(temp / factor);
        remainder = temp % factor;
        temp = attolo + remainder * attos_per_second_sqrt;
        const auto reslo = static_cast<std::uint32_t>(temp / factor);
        remainder = temp % factor;
        t.attoseconds = static_cast<std::int64_t>(reslo) + static_cast<std::int64_t>(reshi) * attos_per_second_sqrt;
        if (remainder >= factor / 2) {
            if (++t.attoseconds >= attos_per_second) {
                t.attoseconds = 0;
                ++t.seconds;
            }
        }
        return t;
    }

    static constexpr Time from_hz(std::uint32_t hz) noexcept {
        return hz > 1 ? Time{0, hz_to_attoseconds(hz)} : hz == 1 ? Time{1, 0} : never();
    }
    static constexpr Time from_usec(std::int64_t usec) noexcept {
        return {static_cast<std::int32_t>(usec / 1'000'000), (usec % 1'000'000) * (attos_per_second / 1'000'000)};
    }
    static constexpr Time from_ticks(std::uint64_t ticks, std::uint32_t frequency) noexcept {
        if (frequency == 0) return never();
        const std::int64_t per_tick = hz_to_attoseconds(frequency);
        if (ticks < frequency) return {0, static_cast<std::int64_t>(ticks) * per_tick};
        return {static_cast<std::int32_t>(ticks / frequency), static_cast<std::int64_t>(ticks % frequency) * per_tick};
    }
    [[nodiscard]] constexpr std::uint64_t as_ticks(std::uint32_t frequency) const noexcept {
        const auto fracticks = static_cast<std::uint32_t>(Time{0, attoseconds}.times(frequency).seconds);
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(seconds)) * frequency + fracticks;
    }
};

} // namespace qilin::mame
