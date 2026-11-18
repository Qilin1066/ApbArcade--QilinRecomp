#pragma once

#include <cstdint>

namespace qilin::system2 {

enum class IrqSource : std::uint8_t {
    sound_command_read = 1, sound_reply_written = 2, scanline = 4, vblank = 8
};

enum class IrqWrite { enable, ack_sound_command, ack_scanline, ack_vblank, unmapped };

[[nodiscard]] constexpr IrqWrite irq_write_port(std::uint16_t address) noexcept {
    if ((address & 0xff81u) == 0x1600) {
        return IrqWrite::enable;
    }
    switch (address & 0xffe1u) {
    case 0x1580: return IrqWrite::ack_sound_command;
    case 0x15c0: return IrqWrite::ack_scanline;
    case 0x15e0: return IrqWrite::ack_vblank;
    default: return IrqWrite::unmapped;
    }
}

class Interrupts {
public:
    constexpr void set_enable(std::uint8_t value) noexcept {
        enabled_ = static_cast<std::uint8_t>(value & 0x0f);
    }

    // Each device event samples its enable bit into a latch. Mask writes leave latched lines alone.
    constexpr void sample(IrqSource source) noexcept {
        const auto bit = static_cast<std::uint8_t>(source);
        pending_ = static_cast<std::uint8_t>((pending_ & ~bit) | (enabled_ & bit));
    }

    constexpr void acknowledge(IrqSource source) noexcept {
        pending_ = static_cast<std::uint8_t>(pending_ & ~static_cast<std::uint8_t>(source));
    }

    [[nodiscard]] constexpr std::uint8_t enabled() const noexcept { return enabled_; }
    [[nodiscard]] constexpr std::uint8_t control_pins() const noexcept { return pending_; }

private:
    std::uint8_t enabled_{};
    std::uint8_t pending_{};
};

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
