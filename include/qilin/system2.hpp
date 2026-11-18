#pragma once

#include <cstdint>

namespace qilin::system2 {

// The low byte of each word from 0x1800 through 0x1BFF strobes the watchdog.
[[nodiscard]] constexpr bool is_watchdog_write(std::uint16_t address) noexcept {
    return (address & 0xfc01u) == 0x1800;
}

class Watchdog {
public:
    // Count in 10 MHz main-board clocks, including while the CPU is idle.
    static constexpr std::uint32_t timeout_ticks = 16u * 16u * 16u * 256u;

    constexpr void advance(std::uint64_t ticks) noexcept {
        ticks_left_ = ticks >= ticks_left_ ? 0 : ticks_left_ - static_cast<std::uint32_t>(ticks);
    }

    constexpr void strobe() noexcept {
        if (!reset_pending()) {
            reset();
        }
    }

    // Call when the host applies the board reset. A strobe cannot cancel it once pending.
    constexpr void reset() noexcept { ticks_left_ = timeout_ticks; }

    [[nodiscard]] constexpr bool reset_pending() const noexcept { return ticks_left_ == 0; }
    [[nodiscard]] constexpr std::uint32_t ticks_left() const noexcept { return ticks_left_; }

private:
    std::uint32_t ticks_left_ = timeout_ticks;
};

} // namespace qilin::system2
