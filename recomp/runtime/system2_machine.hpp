#pragma once
// The System 2 board as both CPUs see it, after MAME's atarisy2.cpp: the
// main CPU's map (RAM, palette, ROM banks, the Slapstic-switched video RAM,
// the interrupt latches, IN0, the watchdog, the ADC, the latches to and from
// the sound CPU) and the sound CPU's (RAM, EEPROM, two POKEYs, the YM2151,
// the TMS5220, IN1, the LETA dial, the latches, the IRQ timer), with MAME's
// mirrors. The timed events (the 32V scanline interrupt, VBLANK, the sound
// IRQ, the watchdog) are counted in main-CPU cycles.
//
// The Machine doesn't run the CPUs. A driver does (MAME's cores in the
// reference run, lifted code in the game): it calls the bus functions, keeps
// main_time and sound_time current, runs the events that fall due, and passes
// the line states on to its CPUs.
//
// The sound chips are modelled only as far as the CPU sees them: register
// writes are kept, and reads return what MAME's devices would for a program
// that waits on them (the YM2151's busy flag for 32 sound cycles after a
// write, a POKEY's RANDOM, the switches on ALLPOT). Their audio comes later.

#include "system2.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace qilin::sys2 {

// Event timing, in main-CPU cycles (10 MHz): all whole numbers.
inline constexpr std::int64_t cycles_per_scanline = 400;   // 640 pixels at 16 MHz
inline constexpr std::int64_t cycles_per_frame = cycles_per_scanline * vtotal;   // 166,400: 60.1 Hz
inline constexpr std::int64_t sound_irq_period = 16 * 16 * 16 * 10;              // 40,960: 244 Hz
inline constexpr std::int64_t watchdog_period = 16 * 16 * 16 * 256;              // 1,048,576
inline constexpr unsigned vblank_line = visible_height;                          // 384

// The sound CPU's clock against the main CPU's: MAME's 14,318,181 Hz / 8 against 10 MHz.
inline constexpr std::int64_t sound_clock_num = 14'318'181;
inline constexpr std::int64_t sound_clock_den = 80'000'000;
[[nodiscard]] constexpr std::int64_t sound_cycles_at(std::int64_t main_cycles) noexcept {
    return main_cycles * sound_clock_num / sound_clock_den;
}

// The switches and controls, as the ports read them. Active-low bits read 1 when idle.
struct Inputs {
    std::uint16_t in0 = 0xffff;   // bits 4-5 are the latches' and are replaced
    std::uint8_t in1 = 0xf0;      // self-test (bit 4), service (5), coins (6, 7); bits 0-3 are the board's
    std::uint8_t dsw0 = 0x00, dsw1 = 0x00;
    std::array<std::uint8_t, 4> adc{0xff, 0x00, 0xff, 0xff};    // APB: ADC1 is the pedal
    std::array<std::uint8_t, 4> leta{0x00, 0xff, 0xff, 0xff};   // APB: LETA0 is the steering dial
};

// A generic_latch_8: data and a pending flag that reading clears.
struct Latch {
    std::uint8_t data = 0;
    bool pending = false;
};

class Pokey {
public:
    std::array<std::uint8_t, 16> written{};
    std::uint8_t read(unsigned offset, std::uint8_t allpot, std::int64_t sound_time) const noexcept;
};

class Machine {
public:
    Machine(std::span<const std::uint8_t> main_rom, std::span<const std::uint8_t> sound_rom);

    // The board's reset (power-on, or the watchdog): MAME's machine_reset.
    // The CPUs are the driver's to reset.
    void reset();

    Inputs inputs;

    // ---- Main CPU bus. Every access, fetches included, passes the Slapstic. ----
    std::uint16_t main_fetch(std::uint16_t address);
    std::uint16_t main_read16(std::uint16_t address);
    void main_write16(std::uint16_t address, std::uint16_t value);
    std::uint8_t main_read8(std::uint16_t address);
    void main_write8(std::uint16_t address, std::uint8_t value);
    [[nodiscard]] std::uint8_t bank(int window) const noexcept { return bank_[window]; }
    // The two bank registers, for lifted code to compare against its own bank.
    [[nodiscard]] std::uint8_t* bank_registers() noexcept { return bank_; }

    // ---- Sound CPU bus. ----
    std::uint8_t sound_read(std::uint16_t address);
    void sound_write(std::uint16_t address, std::uint8_t value);

    // ---- Lines to the CPUs. ----
    [[nodiscard]] std::uint8_t main_cp_lines() const noexcept;   // CP0-CP3 in bits 0-3
    [[nodiscard]] bool sound_held_in_reset() const noexcept { return sound_reset_; }
    [[nodiscard]] bool sound_irq_line() const noexcept { return sound_irq_; }
    [[nodiscard]] bool sound_nmi_line() const noexcept { return soundlatch_.pending; }
    // Rising edges of that line so far: a command written to an empty latch.
    // A driver that samples the line between slices can miss a fall and rise
    // in between; the count can't be missed.
    [[nodiscard]] std::uint64_t sound_nmi_edges() const noexcept { return nmi_edges_; }

    // ---- Time. The driver keeps these current. ----
    std::int64_t main_time = 0;    // main-CPU cycles since power-on
    std::int64_t sound_time = 0;   // sound-CPU cycles since power-on

    [[nodiscard]] std::int64_t next_event() const noexcept;
    void run_events(std::int64_t now);              // everything due at or before 'now'
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    bool take_watchdog_expiry() noexcept;           // true once after the watchdog ran out

    // ---- For tools. ----
    // An optional log of the traffic between the CPUs, to compare runs:
    // what happened ('C' command written, 'c' command read, 'R' reply
    // written, 'r' reply read, 'S' sound reset line, 'E' interrupt enable,
    // 'Q' the sound IRQ asserted, 'A' it acknowledged),
    // the value, and the main-CPU time.
    struct LogEntry {
        char what;
        std::uint8_t value;
        std::int64_t time;
    };
    std::vector<LogEntry>* log = nullptr;
    struct Counters {
        std::uint64_t sound_commands = 0, sound_command_reads = 0, sound_replies = 0, sound_reply_reads = 0;
        std::uint64_t sound_resets = 0, watchdog_strobes = 0, eeprom_writes = 0;
    } counters;
    [[nodiscard]] const VideoMemory& video() const noexcept { return video_; }
    [[nodiscard]] const PaletteRam& palette() const noexcept { return palette_; }
    [[nodiscard]] std::uint16_t xscroll() const noexcept { return xscroll_; }
    [[nodiscard]] std::uint16_t yscroll() const noexcept { return yscroll_; }
    [[nodiscard]] std::uint8_t last_sound_command() const noexcept { return soundlatch_.data; }
    [[nodiscard]] std::uint8_t last_sound_reply() const noexcept { return mainlatch_.data; }
    [[nodiscard]] std::uint8_t interrupt_enable() const noexcept { return interrupt_enable_; }
    [[nodiscard]] const std::array<std::uint8_t, 0x1000>& sound_ram() const noexcept { return sound_ram_; }
    [[nodiscard]] const std::array<std::uint8_t, 0x1000>& main_ram() const noexcept { return main_ram_; }

private:
    void main_write(std::uint16_t address, std::uint16_t data, std::uint16_t mask);
    void sound_reset_w(std::uint8_t data);
    void apply_pending_enable();

    std::span<const std::uint8_t> main_rom_;    // MAME's "maincpu" region, 0x90000 bytes
    std::span<const std::uint8_t> sound_rom_;   // MAME's "audiocpu" region, 0x10000 bytes

    // Main board.
    std::array<std::uint8_t, 0x1000> main_ram_{};
    PaletteRam palette_;
    VideoMemory video_;
    std::uint8_t bank_[2] = {0, 0};
    std::uint8_t interrupt_enable_ = 0;
    int pending_enable_ = -1;             // int_enable_w is applied at the next synchronisation, as in MAME
    bool scanline_int_ = false, video_int_ = false, p2portwr_ = false, p2portrd_ = false;
    std::uint16_t xscroll_ = 0, yscroll_ = 0;
    std::uint8_t adc_channel_ = 0;

    // Between the CPUs.
    Latch soundlatch_;   // main -> sound: its pending flag is the 6502's NMI
    std::uint64_t nmi_edges_ = 0;
    Latch mainlatch_;    // sound -> main

    // Sound board.
    std::array<std::uint8_t, 0x1000> sound_ram_{};
    std::array<std::uint8_t, 0x200> eeprom_{};
    Pokey pokey_[2];
    std::uint8_t ym_address_ = 0;
    std::array<std::uint8_t, 256> ym_registers_{};
    std::int64_t ym_busy_until_ = 0;     // sound cycles
    std::uint8_t tms_data_ = 0;
    bool sound_reset_ = true, sound_irq_ = false, sndrst_state_ = false;
    std::uint8_t coin_counters_ = 0, switch_bits_ = 0, mixer_ = 0;

    // Events (main-CPU cycles).
    std::int64_t next_scanline_event_ = 0;   // the 32V timer: lines 0, 64, ... 384 of each frame
    std::int64_t next_sound_irq_ = sound_irq_period;
    std::int64_t watchdog_deadline_ = watchdog_period;
    bool watchdog_expired_ = false;
    std::uint64_t frames_ = 0;
};

} // namespace qilin::sys2
