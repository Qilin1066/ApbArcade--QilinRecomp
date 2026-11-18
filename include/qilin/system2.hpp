#pragma once

#include <array>
#include <cstdint>

namespace qilin::system2 {

[[nodiscard]] constexpr bool is_video_ram(std::uint16_t address) noexcept {
    return address >= 0x2000 && address < 0x4000;
}

// The -110 part drives the System 2 video-memory view. Observe aligned bus cycles
// across the full CPU address space, including instruction fetches.
class VideoMemory {
public:
    constexpr void observe(std::uint16_t address) noexcept {
        address = static_cast<std::uint16_t>(address & 0xfffeu);
        if (address == 0x8000) {
            phase_ = Phase::armed;
            return;
        }

        switch (phase_) {
        case Phase::idle:
            break;
        case Phase::armed:
            if (matches(address, 0x3fcf, 0x0040)) {
                select(static_cast<std::uint8_t>((address >> 5) & 3));
            } else if (((address >> 1) & 0x007f) == 0x002d) {
                phase_ = Phase::alternate_validate;
            } else if (matches(address, 0x3ff0, 0x34c0)) {
                phase_ = Phase::bit_load;
            }
            break;
        case Phase::alternate_validate:
            phase_ = matches(address, 0x3fff, 0x3d14) ? Phase::alternate_select : Phase::armed;
            break;
        case Phase::alternate_select:
            if (matches(address, 0x3ffc, 0x3d24)) {
                staged_ = static_cast<std::uint8_t>((address >> 1) & 3);
                phase_ = Phase::alternate_commit;
            } else {
                phase_ = Phase::armed;
            }
            break;
        case Phase::alternate_commit:
            if (matches(address, 0x3fcf, 0x0040)) {
                select(staged_);
            }
            break;
        case Phase::bit_load:
            if (matches(address, 0x3fcf, 0x0040)) {
                staged_ = bank_;
                phase_ = Phase::bit_odd;
            }
            break;
        case Phase::bit_odd:
        case Phase::bit_even:
            if (matches(address, 0x3ff0, 0x34c0)) {
                const auto code = (address >> 1) & 3;
                const auto operation = phase_ == Phase::bit_odd ? code : 3 - code;
                const auto mask = 1u << (operation / 2);
                staged_ = static_cast<std::uint8_t>((operation & 1) ? staged_ | mask : staged_ & ~mask);
                phase_ = phase_ == Phase::bit_odd ? Phase::bit_even : Phase::bit_odd;
            } else if (matches(address, 0x3ff8, 0x34d0)) {
                select(staged_);
            }
            break;
        }
    }

    [[nodiscard]] constexpr unsigned bank() const noexcept { return bank_; }

    [[nodiscard]] constexpr std::uint16_t read_window(std::uint16_t address) const noexcept {
        return bank_ == 1 ? 0xffff : pages_[page_index()][(address & 0x1fffu) / 2];
    }

    constexpr void write_window(std::uint16_t address, std::uint16_t value) noexcept {
        if (bank_ != 1) {
            pages_[page_index()][(address & 0x1fffu) / 2] = value;
        }
    }

private:
    enum class Phase {
        idle, armed, alternate_validate, alternate_select, alternate_commit, bit_load, bit_odd, bit_even
    };

    [[nodiscard]] static constexpr bool matches(std::uint16_t address, unsigned mask, unsigned value) noexcept {
        return (address & (0xfe00u | (mask << 1))) == (0x8000u | (value << 1));
    }

    constexpr void select(std::uint8_t bank) noexcept {
        bank_ = bank;
        phase_ = Phase::idle;
    }

    [[nodiscard]] constexpr unsigned page_index() const noexcept {
        return bank_ == 0 ? 0 : bank_ - 1;
    }

    std::array<std::array<std::uint16_t, 0x1000>, 3> pages_{};
    Phase phase_ = Phase::idle;
    std::uint8_t bank_ = 3;
    std::uint8_t staged_{};
};

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
