#include "mame_mode.hpp"

#include <algorithm>
#include <stdexcept>

namespace qilin::game {

namespace {

using mame::Time;

// atarisy2.cpp's clocks, as MAME's devices get them (an XTAL's integer value).
constexpr std::uint32_t main_hz = 10'000'000;    // MASTER_CLOCK/2
constexpr std::uint32_t sound_hz = 1'789'772;    // SOUND_CLOCK/8: 14318181/8 = 1789772.625, truncated
constexpr std::uint32_t ym_hz = 3'579'545;       // SOUND_CLOCK/4
constexpr std::uint32_t adc_hz = 625'000;        // MASTER_CLOCK/32

// The screen: set_raw(VIDEO_CLOCK/2, 640, 0, 512, 416, 0, 384).
constexpr std::int64_t pixeltime = 62'500'000'000;             // HZ_TO_ATTOSECONDS(16 MHz)
constexpr std::int64_t frame_period = pixeltime * 640 * 416;    // m_refresh
constexpr std::int64_t scantime = frame_period / 416;
constexpr std::int64_t vblank_period = scantime * (416 - 384);
constexpr int screen_height = 416, visible_bottom = 383;

// HZ_TO_ATTOSECONDS of an XTAL's double value: the 6502's IRQ (244.140625 Hz) and the watchdog (9.5367... Hz).
const Time sound_irq_period{0, static_cast<std::int64_t>(1e18 / (20e6 / 2 / 16 / 16 / 16 / 10))};
const Time watchdog_time{0, static_cast<std::int64_t>(1e18 / (20e6 / 2 / 16 / 16 / 16 / 256))};
const Time stream_update{0, mame::hz_to_attoseconds(50)};   // STREAMS_UPDATE_ATTOTIME

// The T-11's CP lines (t11.h), the 6502's lines (m6502.h), as input indices here.
constexpr int t11_cp0 = 0;
constexpr int m6502_irq = 0, m6502_nmi = 1;

// POKEY's poly9 and poly17 RANDOM tables (pokey.cpp poly_init_9_/poly_init_17).
struct PolyTables {
    std::array<std::uint8_t, 511> p9{};
    std::array<std::uint8_t, 131071> p17{};
    PolyTables() {
        std::uint32_t state = 511;
        for (auto& value : p9) {
            state = (state >> 1) | (((state ^ (state >> 5)) & 1) << 8);
            value = static_cast<std::uint8_t>(state);
        }
        state = 131071;
        for (auto& value : p17) {
            const auto bit7 = ((state >> 8) ^ (state >> 13)) & 1;
            const auto bit16 = state & 1;
            state = ((state >> 1) & 0xff7f) | (bit7 << 7) | (bit16 << 16);
            value = static_cast<std::uint8_t>(state >> 8);
        }
    }
};
const PolyTables& poly() {
    static const PolyTables tables;
    return tables;
}

} // namespace

MameRun::MameRun(sys2::Machine& machine, Scheduler& scheduler) : m_(machine), s_(scheduler) {
    bus_.run = this;
    m_.mame = this;
    s_.external = this;

    main_.tag = ":maincpu";
    main_.configure(main_hz, 12);
    main_.icount = &s_.main.icount;
    audio_.tag = ":audiocpu";
    audio_.configure(sound_hz, 1);
    audio_.icount = &cpu6502_.icount();
    cpu6502_.mame0264 = true;
    audio_.run = [this] { cpu6502_.execute_run(); };
    audio_.set_input = [this](int line, int state) {
        cpu6502_.mame_set_input(line == m6502_nmi ? m6502_device::NMI_LINE : m6502_device::IRQ_LINE, state != 0,
                                ms_.time() > Time{});
    };
    audio_.device_reset = [this] { cpu6502_.mame_reset(); };
    audio_.periodic_period = sound_irq_period;
    audio_.periodic = [this] { ms_.set_input_line(audio_, m6502_irq, mame::assert_line); };   // sound_irq_gen
    main_.set_input = [this](int line, int state) {
        // t11_device::execute_set_input
        if (state == mame::clear_line)
            m_.mame_cp_state_ = static_cast<std::uint8_t>(m_.mame_cp_state_ & ~(1u << line));
        else
            m_.mame_cp_state_ = static_cast<std::uint8_t>(m_.mame_cp_state_ | (1u << line));
    };
    for (int i = 0; i < 2; ++i) {
        auto& e = pokey_exec_[static_cast<std::size_t>(i)];
        e.tag = i == 0 ? ":pokey1" : ":pokey2";
        e.configure(sound_hz, 1);
        e.icount = &pokey_[static_cast<std::size_t>(i)].icount;
        e.run = [this, i] { pokey_run(i); };
    }
    // MAME's device order: maincpu, audiocpu, ..., pokey1, pokey2. Every
    // executing device starts suspended for reset and is resumed by its
    // pre-reset (interface_pre_start, interface_pre_reset).
    ms_.devices = {&main_, &audio_, &pokey_exec_[0], &pokey_exec_[1]};
    for (auto* e : ms_.devices) {
        e->suspend = mame::suspend_reset;
        e->nextsuspend = 0;
    }

    m_.speech_.external_io_ready_timer = [this](std::int32_t clocks, std::int32_t usec, std::int32_t param) {
        const std::uint32_t hz = m_.speech_.chip_hz();
        const Time delay = clocks >= 0 ? Time{0, clocks * mame::hz_to_attoseconds(hz)} : Time::from_usec(usec);
        io_ready_->adjust(delay, param);
    };
    // tms5220_device's stream: clock / 80 samples a second (an integer),
    // each sample_attos long, numbered afresh each second (sound.cpp).
    m_.speech_.mame_samples_due = [this] {
        const std::uint32_t rate = m_.speech_.chip_hz() / 80;
        if (rate != 7812) throw std::runtime_error("MAME mode: the speech chip's clock changed (not modelled)");
        const std::int64_t sample_attos = (mame::attos_per_second + rate - 1) / rate;
        const Time t = ms_.time();
        const auto within = std::min<std::int64_t>((t.attoseconds + sample_attos - 1) / sample_attos, rate);
        return static_cast<std::uint64_t>(t.seconds) * rate + static_cast<std::uint64_t>(within);
    };
    m_.speech_.mame_sample_start = [this](std::uint64_t index) {
        const std::uint32_t rate = 7812;
        const std::int64_t sample_attos = (mame::attos_per_second + rate - 1) / rate;
        const Time t{static_cast<std::int32_t>(index / rate), static_cast<std::int64_t>(index % rate) * sample_attos};
        return main_cycles_at(t);
    };
}

MameRun::~MameRun() {
    m_.speech_.mame_samples_due = {};
    m_.speech_.mame_sample_start = {};
    m_.mame = nullptr;
    m_.speech_.external_io_ready_timer = {};
    s_.external = nullptr;
}

// ---- Power-on, in the order MAME sets up its timers ---------------------------

void MameRun::start() {
    // sound_manager's constructor.
    sound_update_ = &ms_.timer_alloc("sound_manager::update", [](std::int32_t) {});
    sound_update_->adjust(stream_update, 0, stream_update);
    ms_.start();   // the first quantum, the perfect interleave, the periodic IRQ timer
    // Device starts.
    adc_timer_ = &ms_.timer_alloc("adc0808_device::update_state", [this](std::int32_t p) { adc_update(p); });
    adc_timer_->adjust(Time{}, 0, Time::from_hz(adc_hz));
    vblank_begin_ = &ms_.timer_alloc("screen_device::vblank_begin", [this](std::int32_t p) { vblank_begin(p); });
    vblank_end_ = &ms_.timer_alloc("screen_device::vblank_end", [this](std::int32_t p) { vblank_end(p); });
    scanline0_ = &ms_.timer_alloc("screen_device::first_scanline_tick", [this](std::int32_t p) { first_scanline_tick(p); });
    vblank_start_time_ = Time{};
    vblank_end_time_ = Time{0, vblank_period};
    vblank_begin_->adjust(time_until_pos(visible_bottom + 1));
    scanline0_->adjust(time_until_pos(0));
    // generic_latch_base_device::device_start: the pending callbacks, once.
    ms_.synchronize("generic_latch_base_device::init_callback", [this](std::int32_t) {
        ms_.set_input_line(audio_, m6502_nmi, m_.soundlatch_.pending ? mame::assert_line : mame::clear_line);
    });
    ms_.synchronize("generic_latch_base_device::init_callback", [](std::int32_t) {});
    force_update_ = &ms_.timer_alloc("atari_motion_objects_device::force_update", [this](std::int32_t p) { force_update(p); });
    force_update_->adjust(time_until_pos(0));
    autoboot_ = &ms_.timer_alloc("mame_machine_manager::autoboot_callback", [](std::int32_t) {});
    autoboot_->adjust(Time{});
    scantimer_ = &ms_.timer_alloc("timer_device::scanline_tick", [this](std::int32_t p) { scanline_tick(p); });
    watchdog_ = &ms_.timer_alloc("watchdog_timer_device::watchdog_expired", [this](std::int32_t) { watchdog_fired_ = true; });
    io_ready_ = &ms_.timer_alloc("tms5220_device::set_io_ready", [this](std::int32_t param) {
        m_.speech_.fire_io_ready(param, main_cycles_at(ms_.time()));
    });
    yscroll_timer_ = &ms_.timer_alloc("atarisy2_state::reset_yscroll_callback", [this](std::int32_t param) {
        m_.playfield_scroll_y_ = param;
    });
    // Device resets.
    ms_.synchronize("ym_generic_device::fm_mode_write", [](std::int32_t) {});   // the YM2151's reset writes its mode register
    audio_.periodic_timer->adjust(audio_.periodic_period, 0, audio_.periodic_period);   // the 6502's post-reset
    scantimer_first_ = true;
    scantimer_->adjust(Time{});
    watchdog_->adjust(watchdog_time);
    ms_.synchronize("ym_generic_device::fm_mode_write", [](std::int32_t) {});
    // POKEYs' device_reset: pokey_potgo.
    for (auto& p : pokey_) {
        p.allpot = 0;
        p.pot_counter = 0;
        p.potx.fill(228);
    }
    // atarisy2_state::machine_reset: interrupt enable 0, sound_reset_w(1).
    m_.interrupt_enable_ = 0;
    m_.pending_enable_ = -1;
    m_.sound_reset_w(1);
    begin_main_run();
}

// ---- The main CPU's runs -----------------------------------------------------------

void MameRun::begin_main_run() {
    ms_.run_until_main();
    m_.main_time = main_.clock_cycles;
    check_stop();
}

void MameRun::end_main_run() {
    ms_.main_ran();
    m_.main_time = main_.clock_cycles;
}

void MameRun::check_stop() {
    if (watchdog_fired_) throw Stop{Stop::watchdog, "the watchdog ran out at frame " + std::to_string(m_.frames())};
    s_.external_frame_check();
}

void MameRun::main_service() {
    end_main_run();
    begin_main_run();
    // t11_device::execute_run: interrupts at the start of each run.
    if (s_.take_interrupt_at_run_start() && s_.main.icount <= 0) main_service();
}

void MameRun::main_wait() {
    // WAIT sets the icount to 0; each later run takes an interrupt or does nothing.
    for (;;) {
        s_.main.icount = 0;
        end_main_run();
        begin_main_run();
        if (s_.take_interrupt_at_run_start() || s_.main_interrupt_was_taken()) return;
    }
}

// ---- The sound CPU's bus -----------------------------------------------------------

void MameRun::SoundBus::now() {
    run->m_.sound_time = run->audio_.clock_cycles + (run->audio_.cycles_running - run->cpu6502_.icount());
}
std::uint8_t MameRun::SoundBus::read(std::uint16_t a) {
    now();
    return run->m_.sound_read(a);
}
std::uint8_t MameRun::SoundBus::read_sync(std::uint16_t a) {
    now();
    return run->m_.sound_read(a);
}
std::uint8_t MameRun::SoundBus::read_arg(std::uint16_t a) {
    now();
    return run->m_.sound_read(a);
}
void MameRun::SoundBus::write(std::uint16_t a, std::uint8_t v) {
    now();
    run->m_.sound_write(a, v);
}

// ---- Lines and latches -------------------------------------------------------------

void MameRun::update_interrupts() {
    ms_.set_input_line(main_, t11_cp0 + 3, m_.video_int_ ? mame::assert_line : mame::clear_line);
    ms_.set_input_line(main_, t11_cp0 + 2, m_.scanline_int_ ? mame::assert_line : mame::clear_line);
    ms_.set_input_line(main_, t11_cp0 + 1, m_.p2portwr_ ? mame::assert_line : mame::clear_line);
    ms_.set_input_line(main_, t11_cp0 + 0, m_.p2portrd_ ? mame::assert_line : mame::clear_line);
}

void MameRun::int_enable_w(std::uint8_t data) {
    ms_.synchronize("atarisy2_state::delayed_int_enable_w", [this](std::int32_t param) {
        m_.interrupt_enable_ = static_cast<std::uint8_t>(param);
    }, data);
}

void MameRun::soundlatch_w(std::uint8_t data) {
    ms_.synchronize("generic_latch_8_device::sync_callback", [this](std::int32_t param) {
        m_.soundlatch_.data = static_cast<std::uint8_t>(param);
        if (!m_.soundlatch_.pending) {
            m_.soundlatch_.pending = true;
            ++m_.nmi_edges_;
            // data_pending_callback: the 6502's NMI, and the perfect quantum.
            ms_.set_input_line(audio_, m6502_nmi, mame::assert_line);
            ms_.perfect_quantum(Time::from_usec(200));
        }
    }, data);
}

void MameRun::soundlatch_read() {
    if (m_.soundlatch_.pending) ms_.set_input_line(audio_, m6502_nmi, mame::clear_line);
}

void MameRun::mainlatch_w(std::uint8_t data) {
    ms_.synchronize("generic_latch_8_device::sync_callback", [this](std::int32_t param) {
        m_.mainlatch_.data = static_cast<std::uint8_t>(param);
        m_.mainlatch_.pending = true;
    }, data);
}

void MameRun::sound_reset_line(bool asserted) {
    ms_.set_input_line(audio_, mame::input_line_reset, asserted ? mame::assert_line : mame::clear_line);
}

void MameRun::sound_irq_ack() { ms_.set_input_line(audio_, m6502_irq, mame::clear_line); }

void MameRun::watchdog_reset() { watchdog_->adjust(watchdog_time); }

// ---- The ADC0809 (adc0808.cpp) -------------------------------------------------------

void MameRun::adc_start(unsigned channel) {
    // address_offset_start_w: start_w(1), address_w(offset), start_w(0).
    if (!adc_start_line_) {
        adc_state_ = Adc::start;
        adc_timer_->adjust(Time::from_hz(adc_hz));
    }
    adc_start_line_ = true;
    adc_address_ = static_cast<std::uint8_t>(channel & 7);
    adc_timer_->adjust(Time::from_hz(adc_hz));
    adc_start_line_ = false;
}

void MameRun::adc_update(std::int32_t) {
    auto sample = [this] {
        return adc_address_ < m_.inputs.adc.size() ? m_.inputs.adc[adc_address_] : std::uint8_t{0xff};
    };
    switch (adc_state_) {
    case Adc::idle: adc_timer_->adjust(Time::never()); return;
    case Adc::ready:
        adc_state_ = Adc::running;
        adc_sar_ = sample();
        adc_timer_->adjust(Time::from_ticks(64, adc_hz));
        return;
    case Adc::start:
        adc_state_ = Adc::ready;
        adc_timer_->adjust(Time::from_hz(adc_hz));
        return;
    case Adc::running:
        adc_state_ = Adc::idle;
        adc_sar_ = sample();
        adc_timer_->adjust(Time::never());
        return;
    }
}

// ---- The screen ------------------------------------------------------------------------

std::int64_t MameRun::main_cycles_at(Time t) const {
    return static_cast<std::int64_t>(t.seconds) * main_hz + t.attoseconds / main_.attoseconds_per_cycle;
}

std::int64_t MameRun::sound_cycles_at(Time t) const {
    const std::int64_t aps = audio_.attoseconds_per_cycle;
    const std::int64_t k = mame::attos_per_second / aps, r = mame::attos_per_second % aps;
    return t.seconds * k + (t.seconds * r + t.attoseconds) / aps;
}

int MameRun::vpos() {
    std::int64_t delta = (ms_.time() - vblank_start_time_).as_attoseconds();
    delta += pixeltime / 2;
    const auto v = static_cast<int>(delta / scantime);
    return (visible_bottom + 1 + v) % screen_height;
}

Time MameRun::time_until_pos(int vpos) const {
    vpos += screen_height - (visible_bottom + 1);
    vpos %= screen_height;
    std::int64_t targetdelta = static_cast<std::int64_t>(vpos) * scantime;
    const std::int64_t curdelta = (ms_.time() - vblank_start_time_).as_attoseconds();
    if (targetdelta <= curdelta + pixeltime / 2) targetdelta += frame_period;
    while (targetdelta <= curdelta) targetdelta += frame_period;
    return {0, targetdelta - curdelta};
}

void MameRun::vblank_begin(std::int32_t) {
    vblank_start_time_ = ms_.time();
    vblank_end_time_ = vblank_start_time_ + Time{0, vblank_period};
    // video_manager::frame_update (VIDEO_UPDATE_BEFORE_VBLANK): the rest of
    // the picture, then the frame callbacks (where controls change).
    if (m_.video_sink) m_.video_sink->vblank(m_);
    ++m_.frames_;
    if (m_.log) m_.log->push_back({'F', m_.sound_ram_[0x02], main_cycles_at(ms_.time())});
    if (m_.on_vblank) m_.on_vblank(m_.frames_);
    // screen_vblank(1): atarisy2_state::vblank_int.
    m_.video_int_ = (m_.interrupt_enable_ & 8) != 0;
    update_interrupts();
    vblank_begin_->adjust(time_until_pos(visible_bottom + 1));
    // time_until_vblank_end: in vblank now, so the end of this one.
    vblank_end_->adjust(vblank_end_time_ - ms_.time());
}

void MameRun::vblank_end(std::int32_t) {}

void MameRun::first_scanline_tick(std::int32_t) { scanline0_->adjust(time_until_pos(0)); }

void MameRun::scanline_tick(std::int32_t) {
    int next_vpos = 0;
    if (!scantimer_first_) {
        const int v = vpos();
        // atarisy2_state::scanline_update: the 32V interrupt.
        if (v <= screen_height && v % 64 == 0) {
            m_.scanline_int_ = (m_.interrupt_enable_ & 4) != 0;
            update_interrupts();
        }
        if (v + 64 < screen_height) next_vpos = v + 64;
    }
    scantimer_first_ = false;
    scantimer_->adjust(time_until_pos(next_vpos));
}

void MameRun::force_update(std::int32_t param) {
    if (param > 0 && m_.video_sink) m_.video_sink->draw_through(m_, param - 1);
    param += 64;
    if (param >= visible_bottom) param = 0;
    force_update_->adjust(time_until_pos(param), param);
}

void MameRun::yscroll_reset(int scroll) { yscroll_timer_->adjust(time_until_pos(0), scroll); }

// ---- POKEY (pokey.cpp) ------------------------------------------------------------------

void MameRun::pokey_run(int which) {
    auto& p = pokey_[static_cast<std::size_t>(which)];
    do {
        if (p.skctl & 3) {
            if (++p.p4 == 0x0f) p.p4 = 0;
            if (++p.p5 == 0x1f) p.p5 = 0;
            if (++p.p9 == 0x1ff) p.p9 = 0;
            if (++p.p17 == 0x1ffff) p.p17 = 0;
            if (++p.clock_cnt[1] >= 28) p.clock_cnt[1] = 0;
            bool clk114 = false;
            if (++p.clock_cnt[2] >= 114) {
                p.clock_cnt[2] = 0;
                clk114 = true;
            }
            if ((clk114 || (p.skctl & 0x04)) && p.pot_counter < 228) pokey_step_pot(which);
        }
        --p.icount;
    } while (p.icount > 0);
}

void MameRun::pokey_step_pot(int which) {
    auto& p = pokey_[static_cast<std::size_t>(which)];
    ++p.pot_counter;
    std::uint8_t upd = 0;
    for (int pot = 0; pot < 8; ++pot)
        if (p.potx[static_cast<std::size_t>(pot)] < p.pot_counter || p.pot_counter == 228) upd = static_cast<std::uint8_t>(upd | (1 << pot));
    if (upd)
        ms_.synchronize("pokey_device::sync_pot", [this, which](std::int32_t param) {
            pokey_[static_cast<std::size_t>(which)].allpot |= static_cast<std::uint8_t>(param);
        }, upd);
}

std::uint8_t MameRun::pokey_read(int which, unsigned reg) {
    ms_.synchronize();   // pokey_device::read: "force resync"
    auto& p = pokey_[static_cast<std::size_t>(which)];
    reg &= 15;
    if (reg < 8) return (p.allpot & (1u << reg)) ? p.potx[reg] : p.pot_counter;
    switch (reg) {
    case 0x08:   // ALLPOT: System 2's DIP switches, through allpot_r
        if ((p.skctl & 3) == 0) return p.allpot;
        return p.allpot = which == 0 ? m_.inputs.dsw0 : m_.inputs.dsw1;
    case 0x09: return 0x09;   // KBCODE: 'no key'
    case 0x0a: return (p.audctl & 0x80) ? poly().p9[p.p9] : poly().p17[p.p17];
    case 0x0d: return 0x00;   // SERIN
    case 0x0e: return 0xff ^ 0x08;   // IRQST: SEROC set at start, active low
    case 0x0f: return 0xff;   // SKSTAT
    default: return 0xff;
    }
}

void MameRun::pokey_write(int which, unsigned reg, std::uint8_t value) {
    ms_.synchronize("pokey_device::sync_write", [this, which](std::int32_t param) {
        const auto r = static_cast<unsigned>((param >> 8) & 0xff);
        const auto v = static_cast<std::uint8_t>(param);
        pokey_write_internal(which, r, v);
        m_.pokey_[static_cast<std::size_t>(which)].written[r & 15] = v;
        if (m_.sound_sink) m_.sound_sink->pokey_write(which, r & 15, v, sound_cycles_at(ms_.time()));
    }, static_cast<std::int32_t>((reg << 8) | value));
}

void MameRun::pokey_write_internal(int which, unsigned reg, std::uint8_t value) {
    auto& p = pokey_[static_cast<std::size_t>(which)];
    switch (reg & 15) {
    case 0x08: p.audctl = value; break;   // AUDCTL
    case 0x0b:                            // POTGO
        if (p.skctl & 3) {
            p.allpot = 0;
            p.pot_counter = 0;
            p.potx.fill(228);
        }
        break;
    case 0x0f:   // SKCTL
        if (value == p.skctl) return;
        p.skctl = value;
        if (!(value & 3)) {
            p.p9 = p.p17 = p.p4 = p.p5 = 0;
            p.clock_cnt = {};
        }
        break;
    default: break;
    }
}

// ---- The YM2151 (ymfm via ymopm.cpp) ---------------------------------------------------

bool MameRun::ym_busy() { return ms_.time() < ym_busy_end_; }

void MameRun::ym_data_written() {
    if (!ym_reset_state_) return;   // ym2151_device::write: ignored while held in reset
    if (m_.ym_address_ == 0x14) {
        // The mode register: synchronised (fm_engine_base::write). APB never
        // starts the YM2151's timers; their timers would be MAME timers too.
        if (m_.ym_registers_[0x14] & 0x03) throw std::runtime_error("MAME mode: the YM2151's timers aren't modelled");
        ms_.synchronize("ym_generic_device::fm_mode_write", [](std::int32_t) {}, m_.ym_registers_[0x14]);
    }
    // ym2151::write_data marks it busy after every write, the mode register's too.
    ym_busy_end_ = ms_.time() + Time::from_ticks(64, ym_hz);   // 32 * the prescale (2)
}

void MameRun::ym_reset_line(bool running) {
    // ym2151_device::reset_w: on a change to running, the chip's reset: its
    // status (an interrupt check, synchronised from a running CPU) and its
    // mode register.
    if (running == ym_reset_state_) return;
    ym_reset_state_ = running;
    if (!running) return;
    if (ms_.currently_executing()) ms_.synchronize("ym_generic_device::fm_check_interrupts", [](std::int32_t) {});
    ms_.synchronize("ym_generic_device::fm_mode_write", [](std::int32_t) {});
}

std::int64_t MameRun::speech_time() { return main_cycles_at(ms_.time()); }

} // namespace qilin::game
