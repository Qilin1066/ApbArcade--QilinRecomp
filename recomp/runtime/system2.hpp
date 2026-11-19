#pragma once

// For the few accessors every lifted memory access goes through: inlined even
// into the lifted code's very large functions, where compilers stop inlining.
#if defined(_MSC_VER)
#define QILIN_ALWAYS_INLINE __forceinline
#else
#define QILIN_ALWAYS_INLINE [[gnu::always_inline]] inline
#endif
// Atari System 2 board devices the T-11 sees, as MAME models them
// (src/mame/atari/atarisy2.cpp, atarisy2_v.cpp, slapstic.cpp). Each has been
// checked against MAME's source; see docs/system2.md.
//
// Runtime code: no std::format (macOS 10.15 runtime target), no exceptions.

#include <array>
#include <cstdint>

namespace qilin::sys2 {

// Clocks and video timing (MAME: MASTER_CLOCK 20 MHz, VIDEO_CLOCK 32 MHz).
inline constexpr std::uint32_t main_clock = 10'000'000;      // T-11: MASTER_CLOCK/2
inline constexpr std::uint32_t sound_clock = 1'789'772;      // 6502: 14.318181 MHz/8
inline constexpr std::uint32_t pixel_clock = 16'000'000;     // VIDEO_CLOCK/2
inline constexpr unsigned htotal = 640, vtotal = 416;        // 512 x 384 visible, ROT270 cabinet
inline constexpr unsigned visible_width = 512, visible_height = 384;

// What a main-CPU address decodes to, from MAME's atarisy2_state::main_map
// (written in octal there). Mirrors are folded in.
enum class Area : std::uint8_t {
    ram,            // 0x0000-0x0FFF
    palette,        // 0x1000-0x13FF (0x1000-0x11FF mirrored once)
    adc,            // 0x1400-0x147F: bank select (write), ADC data (read, even byte)
    adc_start,      // 0x1480-0x14FF: ADC address/start, low byte
    int0_ack,       // 0x1580-0x159F: acknowledge the sound-command-read interrupt (CP0)
    sound_reset,    // 0x15A0-0x15BF
    scanline_ack,   // 0x15C0-0x15DF (CP2)
    video_ack,      // 0x15E0-0x15FF (CP3)
    int_enable,     // 0x1600-0x167F
    sound_command,  // 0x1680-0x16FF: the main-to-sound latch
    xscroll,        // 0x1700-0x177F
    yscroll,        // 0x1780-0x17FF
    in0_watchdog,   // 0x1800-0x1BFF: IN0 (read), watchdog (write, even byte)
    sound_reply,    // 0x1C00-0x1FFF: the sound-to-main latch (read)
    video,          // 0x2000-0x3FFF: the Slapstic-selected video RAM view
    rom_window0,    // 0x4000-0x5FFF
    rom_window1,    // 0x6000-0x7FFF
    rom_fixed,      // 0x8000-0xFFFF
    unmapped,       // 0x1500-0x157F: reads 0xFFFF, writes ignored
};

[[nodiscard]] constexpr Area area(std::uint16_t address) noexcept {
    if (address < 0x1000) return Area::ram;
    if (address < 0x1400) return Area::palette;
    if (address < 0x1480) return Area::adc;
    if (address < 0x1500) return Area::adc_start;
    if (address < 0x1580) return Area::unmapped;
    if (address < 0x1600) {
        constexpr std::array<Area, 4> acks{Area::int0_ack, Area::sound_reset, Area::scanline_ack, Area::video_ack};
        return acks[(address >> 5) & 3];
    }
    if (address < 0x1680) return Area::int_enable;
    if (address < 0x1700) return Area::sound_command;
    if (address < 0x1780) return Area::xscroll;
    if (address < 0x1800) return Area::yscroll;
    if (address < 0x1C00) return Area::in0_watchdog;
    if (address < 0x2000) return Area::sound_reply;
    if (address < 0x4000) return Area::video;
    if (address < 0x6000) return Area::rom_window0;
    if (address < 0x8000) return Area::rom_window1;
    return Area::rom_fixed;
}

// Offset of a ROM byte in MAME's 0x90000-byte "maincpu" region. The two
// 8 KiB windows each select one of 64 banks starting at 0x10000.
[[nodiscard]] constexpr std::uint32_t rom_offset(std::uint16_t address, std::uint8_t bank0, std::uint8_t bank1) noexcept {
    if (address >= 0x8000) {
        return address;
    }
    const auto bank = address < 0x6000 ? bank0 : bank1;
    return 0x10000u + bank * 0x2000u + (address & 0x1fffu);
}


inline constexpr unsigned rom_bank_count = 64;

[[nodiscard]] constexpr std::uint8_t select_rom_bank(std::uint16_t word) noexcept {
    // Invert the selector's lowest two bits, then exchange its two low bit pairs.
    const auto selector = static_cast<unsigned>(word >> 10) ^ 3u;
    return static_cast<std::uint8_t>((selector & 0x30u) | ((selector & 0x03u) << 2) | ((selector & 0x0cu) >> 2));
}

[[nodiscard]] constexpr bool is_palette_ram(std::uint16_t address) noexcept {
    return address >= 0x1000 && address < 0x1400;
}

struct Rgb8 {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;

    constexpr bool operator==(const Rgb8&) const noexcept = default;
};

class PaletteRam {
public:
    [[nodiscard]] constexpr std::uint16_t read_word(std::uint16_t address) const noexcept {
        return words_[(address & 0x01ffu) / 2];
    }

    constexpr void write_word(std::uint16_t address, std::uint16_t value) noexcept {
        words_[(address & 0x01ffu) / 2] = value;
    }

    [[nodiscard]] constexpr Rgb8 color(std::uint8_t index) const noexcept {
        const auto raw = words_[index];
        const auto level = static_cast<unsigned>(raw & 0x0f);
        // The shared intensity DAC has a black level and weighted steps.
        const auto intensity = level == 0 ? 0u : 115u + ((level & 8) ? 78u : 0u) +
            ((level & 4) ? 37u : 0u) + ((level & 2) ? 17u : 0u) + ((level & 1) ? 9u : 0u);
        constexpr std::array<unsigned, 16> drive{0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 14, 15, 15};
        const auto component = [&](unsigned shift) {
            return static_cast<std::uint8_t>((drive[(raw >> shift) & 0x0f] * intensity) / 16);
        };
        return {component(12), component(8), component(4)};
    }

private:
    std::array<std::uint16_t, 256> words_{};
};

[[nodiscard]] constexpr bool is_video_ram(std::uint16_t address) noexcept {
    return address >= 0x2000 && address < 0x4000;
}

// The -110 part drives the System 2 video-memory view. Observe aligned bus cycles
// across the full CPU address space, including instruction fetches.
class VideoMemory {
public:
    // The common case (idle, and not the arming address) is a compare; the state machine runs only when armed.
    QILIN_ALWAYS_INLINE constexpr void observe(std::uint16_t address) noexcept {
        if (phase_ == Phase::idle && (address & 0xfffeu) != 0x8000) return;
        step(address);
    }

private:
    constexpr void step(std::uint16_t address) noexcept {
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

public:
    [[nodiscard]] constexpr unsigned bank() const noexcept { return bank_; }
    // The RAM behind the view: page 0 is bank 0 (alphanumerics, then motion
    // objects at word 0xC00), pages 1 and 2 banks 2 and 3 (the playfield).
    [[nodiscard]] constexpr const std::array<std::array<std::uint16_t, 0x1000>, 3>& pages() const noexcept { return pages_; }

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

struct SoundInterface {
    bool cpu_reset = true;
    std::uint8_t chip_reset{}, coin_outputs{}, switch_outputs{};
    std::uint8_t command{}, reply{};
    bool command_pending{}, reply_pending{};

    constexpr void write_reset(std::uint8_t value) noexcept {
        cpu_reset = (value & 1u) != 0;
        chip_reset = coin_outputs = switch_outputs = 0;
    }
    constexpr void write_command(std::uint8_t value) noexcept {
        command = value;
        command_pending = true;
    }
    constexpr std::uint8_t read_command(Interrupts& interrupts) noexcept {
        command_pending = false;
        interrupts.sample(IrqSource::sound_command_read);
        return command;
    }
    constexpr void write_reply(std::uint8_t value, Interrupts& interrupts) noexcept {
        reply = value;
        reply_pending = true;
        interrupts.sample(IrqSource::sound_reply_written);
    }
    constexpr std::uint16_t read_reply(Interrupts& interrupts) noexcept {
        reply_pending = false;
        interrupts.acknowledge(IrqSource::sound_reply_written);
        return static_cast<std::uint16_t>(0xff00u | reply);
    }
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

} // namespace qilin::sys2
