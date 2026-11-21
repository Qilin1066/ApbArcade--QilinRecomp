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
#include "system2_speech.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace qilin::game {
class MameRun;
}

namespace qilin::sys2 {

// The diagnostic MAME-scheduling mode (game/mame_mode.cpp) supplies these:
// with them set, the board's timed and synchronised behaviour is MAME's
// (its timers, set_input_line's queued line changes, the latches' and the
// chips' synchronize calls), driven by a port of MAME's scheduler. Unset,
// as in the player and every other tool, the board runs its own events.
struct MameHooks {
    virtual ~MameHooks() = default;
    virtual void update_interrupts() = 0;   // atarisy2_state::update_interrupts: the four CP lines
    virtual void int_enable_w(std::uint8_t data) = 0;
    virtual void soundlatch_w(std::uint8_t data) = 0;
    virtual void soundlatch_read() = 0;    // the command read: the latch's pending flag drops
    virtual void mainlatch_w(std::uint8_t data) = 0;
    virtual void sound_reset_line(bool asserted) = 0;
    virtual void sound_irq_ack() = 0;
    virtual void watchdog_reset() = 0;
    virtual void adc_start(unsigned channel) = 0;
    [[nodiscard]] virtual std::uint8_t adc_read() = 0;
    virtual void yscroll_reset(int scroll) = 0;   // loaded at line 0
    [[nodiscard]] virtual int vpos() = 0;
    [[nodiscard]] virtual std::uint8_t pokey_read(int which, unsigned reg) = 0;
    virtual void pokey_write(int which, unsigned reg, std::uint8_t value) = 0;
    [[nodiscard]] virtual bool ym_busy() = 0;
    virtual void ym_data_written() = 0;
    virtual void ym_reset_line(bool running) = 0;   // sndrst_6502_w: the YM2151's reset line (high: running)
    [[nodiscard]] virtual std::int64_t speech_time() = 0;   // now, in main cycles, for the speech chip
};

// Event timing, in main-CPU cycles (10 MHz): all whole numbers.
inline constexpr std::int64_t cycles_per_scanline = 400;   // 640 pixels at 16 MHz
inline constexpr std::int64_t cycles_per_frame = cycles_per_scanline * vtotal;   // 166,400: 60.1 Hz
inline constexpr std::int64_t sound_irq_period = 16 * 16 * 16 * 10;              // 40,960: 244 Hz
inline constexpr std::int64_t watchdog_period = 16 * 16 * 16 * 256;              // 1,048,576
inline constexpr unsigned vblank_line = visible_height;                          // 384
// MAME's raster starts at the beginning of vblank. Visible line zero is
// reached 32 scanlines (12,800 main cycles) after machine time zero.
inline constexpr std::int64_t raster_origin = (vtotal - visible_height) * cycles_per_scanline;

// The sound CPU's clock against the main CPU's: MAME's 14,318,181 Hz / 8 against 10 MHz.
inline constexpr std::int64_t sound_clock_num = 14'318'181;
inline constexpr std::int64_t sound_clock_den = 80'000'000;
[[nodiscard]] constexpr std::int64_t sound_cycles_at(std::int64_t main_cycles) noexcept {
    constexpr auto safe = std::numeric_limits<std::int64_t>::max() / sound_clock_num;
    if (static_cast<std::uint64_t>(main_cycles) <= static_cast<std::uint64_t>(safe))
        return main_cycles * sound_clock_num / sound_clock_den;
    // Avoid overflowing the multiply after roughly 18 hours of cabinet uptime.
    return (main_cycles / sound_clock_den) * sound_clock_num +
           (main_cycles % sound_clock_den) * sound_clock_num / sound_clock_den;
}
// The first main-CPU cycle at or after a sound-CPU cycle (no overflow for centuries).
[[nodiscard]] constexpr std::int64_t main_cycles_at(std::int64_t sound_cycles) noexcept {
    const std::int64_t q = sound_cycles / sound_clock_num, r = sound_cycles % sound_clock_num;
    return q * sound_clock_den + (r * sound_clock_den + sound_clock_num - 1) / sound_clock_num;
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
    void write(unsigned offset, std::uint8_t value, std::int64_t sound_time) noexcept;
private:
    void advance(std::int64_t sound_time) const noexcept;
    mutable std::int64_t random_time_ = 0;
    mutable unsigned p9_ = 0, p17_ = 0;
};

class Machine {
public:
    Machine(std::span<const std::uint8_t> main_rom, std::span<const std::uint8_t> sound_rom);

    // The board's reset (power-on, or the watchdog): MAME's machine_reset.
    // The CPUs are the driver's to reset.
    void reset();

    Inputs inputs;

    // ---- Main CPU bus. Every access, fetches included, passes the Slapstic. ----
    // Reads of RAM and ROM, nearly all of them, are inline; the rest of the
    // map goes to main_read_io.
    std::uint16_t main_fetch(std::uint16_t address) { return main_read16(address); }
    QILIN_ALWAYS_INLINE std::uint16_t main_read16(std::uint16_t address) {
        video_.observe(address);
        const auto a = static_cast<std::uint16_t>(address & 0xfffe);
        if (a < 0x1000) return static_cast<std::uint16_t>(main_ram_[a] | (main_ram_[a + 1u] << 8));
        if (a >= 0x4000) {
            const auto offset = rom_offset(a, bank_[0], bank_[1]);
            return static_cast<std::uint16_t>(main_rom_[offset] | (main_rom_[offset + 1] << 8));
        }
        return main_read_io(a);
    }
    void main_write16(std::uint16_t address, std::uint16_t value);
    QILIN_ALWAYS_INLINE std::uint8_t main_read8(std::uint16_t address) {
        const auto word = main_read16(address);
        return static_cast<std::uint8_t>((address & 1) ? word >> 8 : word);
    }
    void main_write8(std::uint16_t address, std::uint8_t value);
    [[nodiscard]] std::uint8_t bank(int window) const noexcept { return bank_[window]; }
    // The two bank registers, for lifted code to compare against its own bank.
    [[nodiscard]] std::uint8_t* bank_registers() noexcept { return bank_; }

    // ---- Sound CPU bus. ROM and RAM reads inline, the rest sound_read_io. ----
    QILIN_ALWAYS_INLINE std::uint8_t sound_read(std::uint16_t address) {
        if (address >= 0x4000) return sound_rom_[address];
        const unsigned m = address & 0x1fffu;   // 0x2000-0x3FFF mirrors everything below
        if (m < 0x1000) return sound_ram_[m];
        return sound_read_io(address);
    }
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
    // Optional: every sound-CPU access to its chips and latches (0x1800-0x1FFF
    // and the mirror), 'r' or 'w', with the sound cycle. For comparing a run
    // with MAME's, access by access (qilin_run --io-trace).
    std::function<void(char kind, std::uint16_t address, std::uint8_t value, std::int64_t sound_time)> io_trace;
    std::function<void(std::uint16_t address, std::uint16_t value, std::int64_t main_time)> input_trace;

    // Optional: told when the picture must be drawn up to a scanline, where
    // MAME forces a partial update (before a changed scroll register or a
    // write to motion object 0's link, and every 64 lines: atarimo.cpp's
    // force_update), and at the start of vblank, where
    // MAME draws the rest of the frame (VIDEO_UPDATE_BEFORE_VBLANK).
    struct VideoSink {
        virtual ~VideoSink() = default;
        virtual void draw_through(const Machine& machine, int scanline) = 0;
        virtual void vblank(const Machine& machine) = 0;
    };
    VideoSink* video_sink = nullptr;

    // The diagnostic MAME-scheduling mode, when set (see MameHooks).
    MameHooks* mame = nullptr;

    // Optional: called at the start of each vblank, on the exact cycle the
    // frame count goes up (after the video sink), with the new count. Tools
    // change the controls here, so every run sees them change at the same moment.
    std::function<void(std::uint64_t frames)> on_vblank;

    // Optional: told of every write that changes the sound, with the sound
    // cycle it happens on (the audio model makes the sound up to it first).
    struct SoundSink {
        virtual ~SoundSink() = default;
        virtual void pokey_write(int which, unsigned reg, std::uint8_t value, std::int64_t sound_time) = 0;
        virtual void ym_write(bool data, std::uint8_t value, std::int64_t sound_time) = 0;
        virtual void ym_reset(bool running, std::int64_t sound_time) = 0;   // the YM2151's reset line, active low
        virtual void mixer_write(std::uint8_t value, std::int64_t sound_time) = 0;
        // A speech sample from the TMS5220, starting at a main-CPU cycle.
        virtual void speech_sample(std::int16_t value, std::int64_t main_cycle) = 0;
    };
    SoundSink* sound_sink = nullptr;

    // The speech chip, and bringing it up to a sound-CPU cycle (its samples
    // go to the sound sink). The board does this before every access to it
    // and at each 32V event; a front end does it before taking sound.
    [[nodiscard]] const Speech& speech() const noexcept { return speech_; }
    void advance_speech(std::int64_t to_sound_time) { speech_.advance(main_cycles_at(to_sound_time)); }
    // The scanline the beam is on, at main_time.
    [[nodiscard]] int vpos() const noexcept {
        if (mame) return mame->vpos();
        return static_cast<int>(((main_time + cycles_per_frame - raster_origin) % cycles_per_frame) / cycles_per_scanline);
    }
    struct Counters {
        std::uint64_t sound_commands = 0, sound_command_reads = 0, sound_replies = 0, sound_reply_reads = 0;
        std::uint64_t sound_resets = 0, watchdog_strobes = 0, eeprom_writes = 0, speech_bytes = 0;
    } counters;
    [[nodiscard]] const VideoMemory& video() const noexcept { return video_; }
    [[nodiscard]] const PaletteRam& palette() const noexcept { return palette_; }
    // The 2804 EEPROM (high scores, settings, audits): loaded and saved by the game's front end.
    [[nodiscard]] std::array<std::uint8_t, 0x200>& eeprom() noexcept { return eeprom_; }
    [[nodiscard]] std::uint16_t xscroll() const noexcept { return xscroll_; }
    // The playfield as MAME's tilemap scrolls it: X from the scroll register
    // (bits 6-15); Y from the register less the scanline it was written on
    // (bit 4 clear), or loaded at line 0 (bit 4 set). The low 4 bits of each
    // register are a playfield tile bank.
    [[nodiscard]] int playfield_scroll_x() const noexcept { return xscroll_ >> 6; }
    [[nodiscard]] int playfield_scroll_y() const noexcept { return playfield_scroll_y_; }
    [[nodiscard]] unsigned playfield_tile_bank(int which) const noexcept { return (which ? yscroll_ : xscroll_) & 0x0f; }
    [[nodiscard]] std::uint16_t yscroll() const noexcept { return yscroll_; }
    [[nodiscard]] std::uint8_t last_sound_command() const noexcept { return soundlatch_.data; }
    [[nodiscard]] std::uint8_t last_sound_reply() const noexcept { return mainlatch_.data; }
    [[nodiscard]] std::uint8_t interrupt_enable() const noexcept { return interrupt_enable_; }
    [[nodiscard]] const std::array<std::uint8_t, 0x1000>& sound_ram() const noexcept { return sound_ram_; }
    [[nodiscard]] const std::array<std::uint8_t, 0x1000>& main_ram() const noexcept { return main_ram_; }

private:
    friend class game::MameRun;
    std::uint8_t mame_cp_state_ = 0;   // MAME mode: the T-11's CP lines, as MAME's queued changes leave them

    std::uint16_t main_read_io(std::uint16_t address);   // 0x1000-0x3FFF, word-aligned, after the Slapstic
    std::uint8_t sound_read_io(std::uint16_t address);   // the EEPROM and the chips
    std::uint8_t sound_read_chips(std::uint16_t address);
    void main_write(std::uint16_t address, std::uint16_t data, std::uint16_t mask);
    void sound_reset_w(std::uint8_t data);
    void apply_pending_enable();
    void advance_adc(std::int64_t now);

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
    int playfield_scroll_y_ = 0;
    int pending_scroll_y_ = -1;   // loaded at line 0
    std::uint8_t adc_channel_ = 0;
    std::uint8_t adc_result_ = 0xff;
    unsigned adc_phase_ = 0;   // idle, start, ready, running (ADC0809 at 625 kHz)
    std::int64_t next_adc_event_ = std::numeric_limits<std::int64_t>::max();

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
    void sndrst_w(bool state, std::int64_t at_sound_time);   // MAME's sndrst_6502_w
    void switch_w(std::uint8_t value, std::int64_t at_main_time);   // MAME's switch_6502_w
    Speech speech_;
    std::uint8_t coin_counters_ = 0, switch_bits_ = 0, mixer_ = 0;

    // Events (main-CPU cycles).
    std::int64_t next_scanline_event_ = raster_origin;   // the 32V timer: lines 0, 64, ... 384 of each frame
    std::int64_t next_sound_irq_ = sound_irq_period;
    std::int64_t watchdog_deadline_ = watchdog_period;
    bool watchdog_expired_ = false;
    std::uint64_t frames_ = 0;
};

} // namespace qilin::sys2
