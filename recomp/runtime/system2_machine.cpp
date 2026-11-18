#include "system2_machine.hpp"

#include <algorithm>

namespace qilin::sys2 {

namespace {

// Main-CPU byte lanes: the T-11 is little-endian, so an even byte is the low lane.
constexpr std::uint16_t low_lane = 0x00ff;

std::uint16_t combine(std::uint16_t old_value, std::uint16_t data, std::uint16_t mask) {
    return static_cast<std::uint16_t>((old_value & ~mask) | (data & mask));
}

} // namespace

std::uint8_t Pokey::read(unsigned offset, std::uint8_t allpot, std::int64_t sound_time) const noexcept {
    switch (offset & 15) {
    case 0x08:   // ALLPOT: System 2 wires the DIP switches here
        return allpot;
    case 0x0a: {
        // RANDOM: MAME's comes from the 17-bit polynomial counter, clocked
        // with the CPU. A hash of the cycle count stands in: the program
        // only needs bytes that vary.
        auto x = static_cast<std::uint64_t>(sound_time) * 0x9e3779b97f4a7c15ull;
        x ^= x >> 29;
        return static_cast<std::uint8_t>(x >> 32);
    }
    case 0x0e:   // IRQST: no POKEY interrupt is pending (active low)
    case 0x0f:   // SKSTAT: no serial or key activity
        return 0xff;
    default:
        return 0x00;
    }
}

Machine::Machine(std::span<const std::uint8_t> main_rom, std::span<const std::uint8_t> sound_rom)
    : main_rom_(main_rom), sound_rom_(sound_rom) {
    eeprom_.fill(0xff);   // MAME's default EEPROM contents, without a saved one
    reset();
}

void Machine::reset() {
    interrupt_enable_ = 0;
    pending_enable_ = -1;
    sound_reset_w(1);
    watchdog_deadline_ = main_time + watchdog_period;
}

// ---- Main CPU -------------------------------------------------------------

std::uint16_t Machine::main_fetch(std::uint16_t address) {
    return main_read16(address);
}

std::uint16_t Machine::main_read16(std::uint16_t address) {
    video_.observe(address);
    address = static_cast<std::uint16_t>(address & 0xfffe);
    switch (area(address)) {
    case Area::ram:
        return static_cast<std::uint16_t>(main_ram_[address] | (main_ram_[address + 1u] << 8));
    case Area::palette:
        return palette_.read_word(address);
    case Area::adc:
        // The ADC0809's result, on the low lane; nothing drives the high one.
        return static_cast<std::uint16_t>(0xff00 | inputs.adc[adc_channel_ & 3]);
    case Area::in0_watchdog:
        return static_cast<std::uint16_t>((inputs.in0 & ~0x0030) | (mainlatch_.pending ? 0x0010 : 0) |
                                          (soundlatch_.pending ? 0x0020 : 0));
    case Area::sound_reply:
        // sound_r: reading the sound CPU's reply clears CP1 and the latch.
        p2portwr_ = false;
        mainlatch_.pending = false;
        ++counters.sound_reply_reads;
        if (log) log->push_back({'r', mainlatch_.data, main_time});
        return static_cast<std::uint16_t>(0xff00 | mainlatch_.data);
    case Area::video:
        return video_.read_window(address);
    case Area::rom_window0:
    case Area::rom_window1:
    case Area::rom_fixed: {
        const auto offset = rom_offset(address, bank_[0], bank_[1]);
        return static_cast<std::uint16_t>(main_rom_[offset] | (main_rom_[offset + 1] << 8));
    }
    default:
        return 0xffff;   // write-only registers and unmapped space read high
    }
}

std::uint8_t Machine::main_read8(std::uint16_t address) {
    const auto word = main_read16(address);
    return static_cast<std::uint8_t>((address & 1) ? word >> 8 : word);
}

void Machine::main_write16(std::uint16_t address, std::uint16_t value) {
    main_write(static_cast<std::uint16_t>(address & 0xfffe), value, 0xffff);
}

void Machine::main_write8(std::uint16_t address, std::uint8_t value) {
    const bool high = (address & 1) != 0;
    main_write(static_cast<std::uint16_t>(address & 0xfffe), static_cast<std::uint16_t>(high ? value << 8 : value),
               high ? 0xff00 : low_lane);
}

void Machine::main_write(std::uint16_t address, std::uint16_t data, std::uint16_t mask) {
    video_.observe(address);
    const bool low = (mask & low_lane) != 0;   // MAME's 8-bit handlers at even addresses see only this lane
    switch (area(address)) {
    case Area::ram: {
        const auto word = combine(static_cast<std::uint16_t>(main_ram_[address] | (main_ram_[address + 1u] << 8)), data, mask);
        main_ram_[address] = static_cast<std::uint8_t>(word);
        main_ram_[address + 1u] = static_cast<std::uint8_t>(word >> 8);
        break;
    }
    case Area::palette:
        palette_.write_word(address, combine(palette_.read_word(address), data, mask));
        break;
    case Area::adc:
        bank_[(address >> 1) & 1] = select_rom_bank(data);
        break;
    case Area::adc_start:
        if (low) adc_channel_ = static_cast<std::uint8_t>((address >> 1) & 7);
        break;
    case Area::int0_ack:
        if (low) p2portrd_ = false;
        break;
    case Area::sound_reset:
        if (low) sound_reset_w(static_cast<std::uint8_t>(data));
        break;
    case Area::scanline_ack:
        if (low) scanline_int_ = false;
        break;
    case Area::video_ack:
        if (low) video_int_ = false;
        break;
    case Area::int_enable:
        if (low) pending_enable_ = data & 0xff;
        break;
    case Area::sound_command:
        if (low) {
            if (!soundlatch_.pending) ++nmi_edges_;
            soundlatch_ = {static_cast<std::uint8_t>(data), true};
            ++counters.sound_commands;
            if (log) log->push_back({'C', soundlatch_.data, main_time});
        }
        break;
    case Area::xscroll: {
        const auto value = combine(xscroll_, data, mask);
        if (video_sink && value != xscroll_) video_sink->draw_through(*this, vpos());
        xscroll_ = value;
        break;
    }
    case Area::yscroll: {
        const auto value = combine(yscroll_, data, mask);
        if (video_sink && value != yscroll_) video_sink->draw_through(*this, vpos());
        yscroll_ = value;
        if (yscroll_ & 0x10) {
            pending_scroll_y_ = yscroll_ >> 6;
        } else {
            playfield_scroll_y_ = (yscroll_ >> 6) - vpos();
        }
        break;
    }
    case Area::in0_watchdog:
        if (low) {
            watchdog_deadline_ = main_time + watchdog_period;
            ++counters.watchdog_strobes;
        }
        break;
    case Area::video:
        // Motion object 0's link word: the start of the list.
        if (video_sink && video_.bank() == 0 && (address & 0x1fff) == 0x1806) video_sink->draw_through(*this, vpos());
        video_.write_window(address, combine(video_.read_window(address), data, mask));
        break;
    default:
        break;   // ROM, the reply latch and unmapped space ignore writes
    }
}

void Machine::sound_reset_w(std::uint8_t data) {
    const bool reset = (data & 1) != 0;
    if (reset && !sound_reset_) ++counters.sound_resets;
    if (log && reset != sound_reset_) log->push_back({'S', static_cast<std::uint8_t>(reset), main_time});
    sound_reset_ = reset;
    // sndrst_6502_w(0), coincount_w(0), switch_6502_w(0)
    sndrst_w(false, sound_cycles_at(main_time));
    coin_counters_ = 0;
    switch_bits_ = 0;
}

void Machine::sndrst_w(bool state, std::int64_t at_sound_time) {
    if (state == sndrst_state_) return;
    sndrst_state_ = state;
    if (sound_sink) sound_sink->ym_reset(state, at_sound_time);
    if (state) {   // 0 -> 1: the TMS5220 reset and mixer_w(0)
        mixer_ = 0;
        if (sound_sink) sound_sink->mixer_write(0, at_sound_time);
    }
}

std::uint8_t Machine::main_cp_lines() const noexcept {
    return static_cast<std::uint8_t>((p2portrd_ ? 1 : 0) | (p2portwr_ ? 2 : 0) | (scanline_int_ ? 4 : 0) | (video_int_ ? 8 : 0));
}

// ---- Sound CPU ------------------------------------------------------------

std::uint8_t Machine::sound_read(std::uint16_t address) {
    if (address >= 0x4000) return sound_rom_[address];
    const unsigned m = address & 0x1fffu;   // 0x2000-0x3FFF mirrors everything below
    if (m < 0x1000) return sound_ram_[m];
    if (m < 0x1800) return eeprom_[m & 0x1ffu];
    const unsigned r = m & 0x7fu;
    switch (r >> 4) {
    case 0: return pokey_[0].read(r, inputs.dsw0, sound_time);
    case 1: return inputs.leta[r & 3];
    case 3: return pokey_[1].read(r, inputs.dsw1, sound_time);
    case 4:   // IN1
        return static_cast<std::uint8_t>((inputs.in1 & 0xf0) | (soundlatch_.pending ? 0x01 : 0) |
                                         (mainlatch_.pending ? 0x02 : 0));   // bit 2: the TMS5220 is ready
    case 5:   // YM2151 status: busy, no timer flags
        return sound_time < ym_busy_until_ ? 0x80 : 0x00;
    case 6:   // sound_6502_r: the main CPU's command; clocks CP0 and drops the NMI
        p2portrd_ = (interrupt_enable_ & 1) != 0;
        soundlatch_.pending = false;
        ++counters.sound_command_reads;
        if (log) log->push_back({'c', soundlatch_.data, main_time});
        return soundlatch_.data;
    default:
        return 0x00;   // unmapped, and the write-only registers at 0x1870-0x187F
    }
}

void Machine::sound_write(std::uint16_t address, std::uint8_t value) {
    if (address >= 0x4000) return;
    const unsigned m = address & 0x1fffu;
    if (m < 0x1000) {
        sound_ram_[m] = value;
        return;
    }
    if (m < 0x1800) {
        eeprom_[m & 0x1ffu] = value;
        ++counters.eeprom_writes;
        return;
    }
    const unsigned r = m & 0x7fu;
    switch (r >> 4) {
    case 0:
    case 3:
        pokey_[r >> 5].written[r & 15] = value;
        if (sound_sink) sound_sink->pokey_write(static_cast<int>(r >> 5), r & 15, value, sound_time);
        return;
    case 5:
        if (sound_sink) sound_sink->ym_write((r & 1) != 0, value, sound_time);
        if (r & 1) {
            ym_registers_[ym_address_] = value;
            ym_busy_until_ = sound_time + 32;   // 64 YM clocks at 3.58 MHz
        } else {
            ym_address_ = value;
        }
        return;
    case 7:
        switch (r & 0x0e) {
        case 0x0: tms_data_ = value; return;
        case 0x2:           // TMS5220 write strobe
            ++counters.speech_bytes;
            return;
        case 0x4:           // sound_6502_w: the reply; clocks CP1
            p2portwr_ = (interrupt_enable_ & 2) != 0;
            mainlatch_ = {value, true};
            ++counters.sound_replies;
            if (log) log->push_back({'R', value, main_time});
            return;
        case 0x6: coin_counters_ = value; return;
        case 0x8:
            if (log) log->push_back({'A', 0, main_time});
            sound_irq_ = false;
            return;
        case 0xa:
            mixer_ = value;
            if (sound_sink) sound_sink->mixer_write(value, sound_time);
            return;
        case 0xc: switch_bits_ = value; return;
        case 0xe:
            sndrst_w((value & 1) != 0, sound_time);
            return;
        default: return;
        }
    default:
        return;   // read-only ports
    }
}

// ---- Events ---------------------------------------------------------------

std::int64_t Machine::next_event() const noexcept {
    return std::min({next_scanline_event_, next_sound_irq_, watchdog_deadline_});
}

void Machine::apply_pending_enable() {
    if (pending_enable_ >= 0) {
        if (log && interrupt_enable_ != pending_enable_) log->push_back({'E', static_cast<std::uint8_t>(pending_enable_), main_time});
        interrupt_enable_ = static_cast<std::uint8_t>(pending_enable_);
        pending_enable_ = -1;
    }
}

void Machine::run_events(std::int64_t now) {
    apply_pending_enable();
    for (;;) {
        const auto t = next_event();
        if (t > now) break;
        if (t == next_scanline_event_) {
            const auto line = static_cast<unsigned>((t % cycles_per_frame) / cycles_per_scanline);
            scanline_int_ = (interrupt_enable_ & 4) != 0;   // the 32V interrupt, every 64 lines
            if (line == 0 && pending_scroll_y_ >= 0) {
                playfield_scroll_y_ = pending_scroll_y_;
                pending_scroll_y_ = -1;
            }
            if (line == vblank_line) {
                if (video_sink) video_sink->vblank(*this);
                video_int_ = (interrupt_enable_ & 8) != 0;
                ++frames_;
                if (log) log->push_back({'F', sound_ram_[0x02], t});   // a frame, with the sound program's status
            }
            next_scanline_event_ = line + 64 <= vblank_line ? t + 64 * cycles_per_scanline
                                                            : t - line * cycles_per_scanline + cycles_per_frame;
        } else if (t == next_sound_irq_) {
            if (!sound_reset_) sound_irq_ = true;   // MAME's periodic interrupt skips a CPU held in reset
            if (log && !sound_reset_) log->push_back({'Q', 0, t});
            next_sound_irq_ += sound_irq_period;
        } else {
            watchdog_expired_ = true;
            watchdog_deadline_ = t + watchdog_period;
        }
    }
}

bool Machine::take_watchdog_expiry() noexcept {
    const bool expired = watchdog_expired_;
    watchdog_expired_ = false;
    return expired;
}

} // namespace qilin::sys2
