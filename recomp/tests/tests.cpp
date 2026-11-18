// Unit tests: no game files needed.
//
// The board models are checked against MAME's own definitions, transcribed
// here independently of recomp/runtime/system2.hpp: MAME's bank table, its
// palette formula on every value, and its Slapstic 103-110 state machine on
// random bus traffic.

#include "cfg.hpp"
#include "romset.hpp"
#include "system2.hpp"
#include "t11_decoder.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

namespace sys2 = qilin::sys2;
namespace t11 = qilin::t11;
namespace rom = qilin::rom;

void test_sha1() {
    const std::string abc = "abc";
    CHECK(rom::sha1({reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()}) ==
          "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(rom::sha1({}) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(rom::sha1({reinterpret_cast<const std::uint8_t*>(two_blocks.data()), two_blocks.size()}) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

void test_rom_set() {
    const auto files = rom::rev7_files();
    CHECK(files.size() == 36);
    // Every region byte is filled by at most one file, and the T-11 program
    // fills the fixed ROM and banks 0-15, 16-31 and 48-63.
    std::array<std::vector<int>, rom::region_count> owner;
    for (std::size_t r = 0; r < rom::region_count; ++r) {
        owner[r].assign(rom::region_info(static_cast<rom::Region>(r)).size, 0);
    }
    for (const auto& file : files) {
        std::uint32_t length = 0;
        for (const auto& piece : file.pieces) {
            for (std::uint32_t i = 0; i < piece.length; ++i) {
                ++owner[static_cast<std::size_t>(file.region)][piece.offset + (file.byte_lane ? 2 * i : i)];
            }
            length += piece.length;
        }
        CHECK(length == file.size);
    }
    bool overlap = false;
    for (const auto& region : owner) {
        for (const int n : region) {
            overlap = overlap || n > 1;
        }
    }
    CHECK(!overlap);
    const auto& main = owner[static_cast<std::size_t>(rom::Region::main_cpu)];
    CHECK(main[0x8000] == 1 && main[0xffff] == 1 && main[0x7fff] == 0);
    CHECK(main[0x10000] == 1 && main[0x4ffff] == 1 && main[0x50000] == 0 && main[0x6ffff] == 0);
    CHECK(main[0x70000] == 1 && main[0x8ffff] == 1);

    // A directory without the files: every one is missing and loading says so.
    const auto empty = std::filesystem::temp_directory_path() / "qilin-tests-no-roms";
    std::filesystem::create_directories(empty);
    for (const auto& c : rom::verify(empty)) {
        CHECK(c.status == rom::FileCheck::Status::missing);
    }
    const auto set = rom::RomSet::load(empty);
    CHECK(!set && set.error().find("0 of 36") != std::string::npos);
}

void test_memory_map() {
    CHECK(sys2::area(0x0fff) == sys2::Area::ram);
    CHECK(sys2::area(0x13fe) == sys2::Area::palette);
    CHECK(sys2::area(0x147e) == sys2::Area::adc);
    CHECK(sys2::area(0x1480) == sys2::Area::adc_start);
    CHECK(sys2::area(0x1500) == sys2::Area::unmapped);
    CHECK(sys2::area(0x1580) == sys2::Area::int0_ack);
    CHECK(sys2::area(0x15bf) == sys2::Area::sound_reset);
    CHECK(sys2::area(0x15c0) == sys2::Area::scanline_ack);
    CHECK(sys2::area(0x15fe) == sys2::Area::video_ack);
    CHECK(sys2::area(0x1600) == sys2::Area::int_enable);
    CHECK(sys2::area(0x16fe) == sys2::Area::sound_command);
    CHECK(sys2::area(0x1700) == sys2::Area::xscroll);
    CHECK(sys2::area(0x1780) == sys2::Area::yscroll);
    CHECK(sys2::area(0x1bfe) == sys2::Area::in0_watchdog);
    CHECK(sys2::area(0x1c00) == sys2::Area::sound_reply);
    CHECK(sys2::area(0x3ffe) == sys2::Area::video);
    CHECK(sys2::area(0x5ffe) == sys2::Area::rom_window0);
    CHECK(sys2::area(0x6000) == sys2::Area::rom_window1);
    CHECK(sys2::area(0x8000) == sys2::Area::rom_fixed);
    CHECK(sys2::rom_offset(0x8000, 5, 6) == 0x8000);
    CHECK(sys2::rom_offset(0x4002, 5, 6) == 0x10000 + 5 * 0x2000 + 2);
    CHECK(sys2::rom_offset(0x7ffe, 5, 63) == 0x10000 + 63 * 0x2000 + 0x1ffe);
}

void test_bank_select() {
    // The table MAME's bankselect_w keeps (commented) beside its bitswap.
    static constexpr std::array<int, 64> mame{
        12, 8, 4, 0, 13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3,
        28, 24, 20, 16, 29, 25, 21, 17, 30, 26, 22, 18, 31, 27, 23, 19,
        44, 40, 36, 32, 45, 41, 37, 33, 46, 42, 38, 34, 47, 43, 39, 35,
        60, 56, 52, 48, 61, 57, 53, 49, 62, 58, 54, 50, 63, 59, 55, 51};
    for (unsigned selector = 0; selector < 64; ++selector) {
        CHECK(sys2::select_rom_bank(static_cast<std::uint16_t>(selector << 10 | 0x3ff)) == mame[selector]);
    }
}

void test_palette() {
    // MAME's atarisy2_state::RRRRGGGGBBBBIIII.
    constexpr int ZB = 115, Z3 = 78, Z2 = 37, Z1 = 17, Z0 = 9;
    constexpr int intensity_table[16] = {0, ZB + Z0, ZB + Z1, ZB + Z1 + Z0, ZB + Z2, ZB + Z2 + Z0, ZB + Z2 + Z1,
                                         ZB + Z2 + Z1 + Z0, ZB + Z3, ZB + Z3 + Z0, ZB + Z3 + Z1, ZB + Z3 + Z1 + Z0,
                                         ZB + Z3 + Z2, ZB + Z3 + Z2 + Z0, ZB + Z3 + Z2 + Z1, ZB + Z3 + Z2 + Z1 + Z0};
    constexpr int color_table[16] = {0x0, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xe, 0xf, 0xf};
    sys2::PaletteRam palette;
    bool all = true;
    for (unsigned raw = 0; raw < 0x10000; ++raw) {
        palette.write_word(0x1000, static_cast<std::uint16_t>(raw));
        const int i = intensity_table[raw & 15];
        const sys2::Rgb8 expected{static_cast<std::uint8_t>((color_table[(raw >> 12) & 15] * i) >> 4),
                                  static_cast<std::uint8_t>((color_table[(raw >> 8) & 15] * i) >> 4),
                                  static_cast<std::uint8_t>((color_table[(raw >> 4) & 15] * i) >> 4)};
        all = all && palette.color(0) == expected;
    }
    CHECK(all);
    // 0x1000-0x11FF is mirrored at 0x1200-0x13FF.
    palette.write_word(0x13fe, 0x1234);
    CHECK(palette.read_word(0x11fe) == 0x1234 && palette.color(255) == palette.color(255));
}

// MAME's atari_slapstic_device for chips 103-110, transcribed from its
// structure (checker, test, and the state classes) with chip 110's data, on
// a 16-bit bus over 0x8000-0x81FF.
class MameSlapstic110 {
public:
    void access(std::uint32_t a) {
        switch (state_) {
        case State::idle:
            if (test_reset(a)) state_ = State::active;
            break;
        case State::active:
            for (int i = 0; i < 4; ++i) {
                if (test_bank(bank_values[i], a)) {
                    current_ = i;
                    state_ = State::idle;
                    return;
                }
            }
            if (test_any(0x007f, 0x002d, a)) state_ = State::alt_valid;
            else if (test_in(0x3ff0, 0x34c0, a)) state_ = State::bit_load;
            break;
        case State::alt_valid:
            if (test_reset(a)) state_ = State::active;
            else if (test_in(0x3fff, 0x3d14, a)) state_ = State::alt_select;
            else state_ = State::active;
            break;
        case State::alt_select:
            if (test_reset(a)) state_ = State::active;
            else if (test_in(0x3ffc, 0x3d24, a)) {
                loaded_ = static_cast<int>((a >> 1) & 3);
                state_ = State::alt_commit;
            } else {
                state_ = State::active;
            }
            break;
        case State::alt_commit:
            if (test_reset(a)) state_ = State::active;
            else if (test_in(0x3fcf, 0x0040, a)) {
                current_ = loaded_;
                state_ = State::idle;
            }
            break;
        case State::bit_load:
            if (test_reset(a)) state_ = State::active;
            else if (test_in(0x3fcf, 0x0040, a)) {
                loaded_ = current_;
                state_ = State::bit_odd;
            }
            break;
        case State::bit_odd:
        case State::bit_even: {
            const bool odd = state_ == State::bit_odd;
            const auto next = odd ? State::bit_even : State::bit_odd;
            // bit3c0 = 0x34c0, bit3s0 = 0x34c1, bit3c1 = 0x34c2, bit3s1 = 0x34c3 (mask 0x3ff3)
            if (test_reset(a)) state_ = State::active;
            else if (test_in(0x3ff3, odd ? 0x34c0 : 0x34c3, a)) { loaded_ &= ~1; state_ = next; }
            else if (test_in(0x3ff3, odd ? 0x34c1 : 0x34c2, a)) { loaded_ |= 1; state_ = next; }
            else if (test_in(0x3ff3, odd ? 0x34c2 : 0x34c1, a)) { loaded_ &= ~2; state_ = next; }
            else if (test_in(0x3ff3, odd ? 0x34c3 : 0x34c0, a)) { loaded_ |= 2; state_ = next; }
            else if (test_in(0x3ff8, 0x34d0, a)) { current_ = loaded_; state_ = State::idle; }
            break;
        }
        }
    }
    int bank() const { return current_; }

private:
    enum class State { idle, active, alt_valid, alt_select, alt_commit, bit_load, bit_odd, bit_even };
    static constexpr std::array<std::uint32_t, 4> bank_values{0x0040, 0x0050, 0x0060, 0x0070};
    static constexpr std::uint32_t range_mask = ~std::uint32_t{0x81ff - 0x8000}, range_value = 0x8000;
    static constexpr std::uint32_t input_mask = 0x3fffu << 1;

    static bool test(std::uint32_t mask, std::uint32_t value, std::uint32_t a) { return (a & mask) == value; }
    static bool test_in(std::uint32_t m, std::uint32_t v, std::uint32_t a) { return test(range_mask | (m << 1), range_value | (v << 1), a); }
    static bool test_any(std::uint32_t m, std::uint32_t v, std::uint32_t a) { return test(m << 1, v << 1, a); }
    static bool test_reset(std::uint32_t a) { return test(range_mask | input_mask, range_value, a); }
    static bool test_bank(std::uint32_t b, std::uint32_t a) { return test(range_mask | input_mask, range_value | (b << 1), a); }

    State state_ = State::idle;
    int current_ = 3;
    int loaded_ = 0;
};

void test_slapstic() {
    // Bus traffic biased towards the addresses the chip decodes, so every
    // sequence (direct, alternate, bitwise) and its breaks occur often.
    std::mt19937 rng(110);
    const auto pick = [&]() -> std::uint16_t {
        static constexpr std::array<std::uint16_t, 16> interesting{
            0x8000, 0x8001, 0x8080, 0x80a0, 0x80c0, 0x80e0, 0x005a, 0x815a,
            0xfa28, 0xfa48, 0xfa4a, 0xfa4c, 0xfa4e, 0xe980, 0xe982, 0xe9a0};
        switch (rng() % 4) {
        case 0: return static_cast<std::uint16_t>(rng());
        case 1: return static_cast<std::uint16_t>(0x8000 + (rng() & 0x1ff));
        case 2: return static_cast<std::uint16_t>(0xe980 + (rng() & 0x7f));
        default: return static_cast<std::uint16_t>(interesting[rng() % interesting.size()] + (rng() & 0x18));
        }
    };
    unsigned changes = 0, mismatches = 0;
    for (int run = 0; run < 200; ++run) {
        sys2::VideoMemory ours;
        MameSlapstic110 mame;
        for (int i = 0; i < 5000; ++i) {
            const auto a = pick();
            const auto before = mame.bank();
            ours.observe(a);
            mame.access(a);
            changes += before != mame.bank();
            mismatches += static_cast<int>(ours.bank()) != mame.bank();
        }
    }
    CHECK(mismatches == 0);
    CHECK(changes > 1000);   // the traffic actually switches banks
    std::printf("slapstic: %u bank changes on random traffic, %u mismatches with MAME's state machine\n", changes, mismatches);

    // View 1 has no RAM: reads are 0xFFFF and writes vanish.
    sys2::VideoMemory video;
    video.write_window(0x2000, 0x1234);
    video.observe(0x8000);
    video.observe(0x80a0);
    CHECK(video.bank() == 1 && video.read_window(0x2000) == 0xffff);
    video.write_window(0x2000, 0x5555);
    video.observe(0x8000);
    video.observe(0x80e0);
    CHECK(video.bank() == 3 && video.read_window(0x2000) == 0x1234);
}

void test_watchdog_and_interrupts() {
    sys2::Watchdog watchdog;
    watchdog.advance(1'048'575);
    CHECK(!watchdog.reset_pending() && watchdog.ticks_left() == 1);
    watchdog.strobe();
    CHECK(watchdog.ticks_left() == 1'048'576);
    watchdog.advance(1'048'576);
    watchdog.strobe();
    CHECK(watchdog.reset_pending());   // a strobe can't cancel an expiry

    sys2::Interrupts irq;
    irq.sample(sys2::IrqSource::scanline);
    CHECK(irq.control_pins() == 0);     // not enabled: the event isn't latched
    irq.set_enable(0xff);
    irq.sample(sys2::IrqSource::scanline);
    irq.sample(sys2::IrqSource::sound_reply_written);
    irq.set_enable(0);
    CHECK(irq.control_pins() == 6);     // disabling leaves latched lines alone
    irq.sample(sys2::IrqSource::scanline);
    CHECK(irq.control_pins() == 2);     // a disabled event clocks a 0 through
    irq.acknowledge(sys2::IrqSource::sound_reply_written);
    CHECK(irq.control_pins() == 0);

    CHECK(sys2::is_watchdog_write(0x1800) && sys2::is_watchdog_write(0x1bfe) && !sys2::is_watchdog_write(0x1801));
    CHECK(sys2::irq_write_port(0x1600) == sys2::IrqWrite::enable && sys2::irq_write_port(0x167e) == sys2::IrqWrite::enable);
    CHECK(sys2::irq_write_port(0x1580) == sys2::IrqWrite::ack_sound_command);
    CHECK(sys2::irq_write_port(0x15de) == sys2::IrqWrite::ack_scanline);
    CHECK(sys2::irq_write_port(0x15e0) == sys2::IrqWrite::ack_vblank);
    CHECK(sys2::irq_write_port(0x1581) == sys2::IrqWrite::unmapped);
}

void test_decoder() {
    // A few encodings checked by hand against the T-11 User's Guide; the full
    // cross-check against MAME is tools/check/t11_decoder_check.cpp.
    auto i = t11::decode(012737, 0340, 0177776);    // MOV #340,@#177776
    CHECK(i.op == t11::Op::mov && i.legal && i.words == 3 && i.src.immediate() && i.dst.absolute());
    CHECK(i.cycles == 12 + 6 + 15);
    CHECK(t11::disassemble(i, 0x8000) == "MOV   #000340,@#177776");
    i = t11::decode(0004736);                       // JSR PC,@(SP)+
    CHECK(i.op == t11::Op::jsr && i.reg == 7 && i.dst.mode == t11::autoinc_deferred && i.dst.reg == 6 && i.words == 1);
    i = t11::decode(0077205);                       // SOB R2, back 5 words
    CHECK(i.op == t11::Op::sob && i.reg == 2 && i.branch_target(0x1000) == 0x1002 - 10);
    i = t11::decode(0000101);                       // JMP R1: traps through 4
    CHECK(i.op == t11::Op::jmp && !i.legal && t11::trap_vector(i) == 4);
    i = t11::decode(0070102);                       // MUL: not on the T-11
    CHECK(i.op == t11::Op::mul && !i.legal && t11::trap_vector(i) == 010);
    i = t11::decode(0106427, 0340);                 // MTPS #340
    CHECK(i.op == t11::Op::mtps && i.byte && i.words == 2 && t11::disassemble(i, 0) == "MTPS  #340");
}

// A synthetic program image: words placed at maincpu-region offsets.
struct Image {
    std::array<std::vector<std::uint8_t>, rom::region_count> data;
    std::array<std::vector<bool>, rom::region_count> populated;
    Image() {
        for (std::size_t r = 0; r < rom::region_count; ++r) {
            data[r].assign(rom::region_info(static_cast<rom::Region>(r)).size, 0);
            populated[r].assign(data[r].size(), false);
        }
        // All program ROM present, as zeros, like the real fixed ROM and code banks.
        for (std::uint32_t i = 0x8000; i < 0x30000; ++i) populated[0][i] = true;
    }
    void put(std::uint32_t offset, std::initializer_list<std::uint16_t> words) {
        for (const auto w : words) {
            data[0][offset] = static_cast<std::uint8_t>(w);
            data[0][offset + 1] = static_cast<std::uint8_t>(w >> 8);
            offset += 2;
        }
    }
    rom::RomSet set() const { return rom::RomSet::from_regions(data, populated); }
};

void test_cfg() {
    namespace cfg = qilin::cfg;
    Image image;
    // Reset: a loop over R5 = 3..0 through a 4-entry table of PC offsets, as
    // APB's start-up does at 0xF15E; then a call to a register-save helper.
    image.put(0x8000, {
        012705, 3,            // 8000 MOV #3,R5
        010500,               // 8004 MOV R5,R0
        006300,               // 8006 ASL R0
        066007, 0x9000,       // 8008 ADD 9000(R0),PC   (PC after = 800C)
        0x0000,               // 800C (never reached: the table covers 0-3)
    });
    // Table at 0x9000: offsets from 0x800C to 0x8010, 0x8014, 0x8018, 0x801C.
    image.put(0x9000, {0x0004, 0x0008, 0x000C, 0x0010});
    image.put(0x8010, {000240, 000401});   // NOP; BR 8016
    image.put(0x8014, {000240, 000240});   // NOP; NOP -> falls into 8018
    image.put(0x8018, {000240, 000240});
    image.put(0x801C, {000240, 005305, 002361});   // NOP; DEC R5; BGE 8004
    image.put(0x8022, {004137, 0x8100, 000207});   // JSR R1,@#8100; RTS PC
    // The helper at 0x8100 (APB's at 0x81E2).
    image.put(0x8100, {010246, 010146, 016601, 4, 004736, 012602, 012601, 000207});
    // Bank 2 runs in window 0: four absolute calls into 0x4xxx, and a
    // function at 0x4100 that opens with JSR R1,@#8100. Its first words are
    // the bank's entry table: 0x4100, then a non-pointer.
    const std::uint32_t bank2 = 0x10000 + 2 * 0x2000;
    image.put(bank2, {0x4100, 0xffff});
    image.put(bank2 + 0x100, {004137, 0x8100, 004737, 0x4200, 004737, 0x4200, 004737, 0x4200, 004737, 0x4200, 000207});
    image.put(bank2 + 0x200, {000137, 0x6000});   // JMP @#6000: window 1's bank is never set
    // Bank 3 is data.
    image.put(0x10000 + 3 * 0x2000, {0x1234, 0x5678});

    const auto set = image.set();
    const auto program = cfg::Program::analyze(set);
    const auto& nodes = program.nodes();
    const auto table = nodes.find(cfg::Loc{0x8008, 0});
    CHECK(table != nodes.end() && table->second.flow == cfg::Flow::table && table->second.table.size() == 4);
    CHECK(nodes.contains(cfg::Loc{0x8010, 0}) && nodes.contains(cfg::Loc{0x801C, 0}) && !nodes.contains(cfg::Loc{0x800C, 0}));
    CHECK(program.save_helpers().contains(0x8100));
    CHECK(program.bank_windows().contains(2) && program.bank_windows().at(2) == 0 && !program.bank_windows().contains(3));
    CHECK(program.prologue_entries().contains(cfg::Loc{0x4100, 2}));
    CHECK(nodes.contains(cfg::Loc{0x4200, 2}));
    bool unknown_bank = false, unexpected = false;
    for (const auto& f : program.findings()) {
        if (f.issue == cfg::Issue::unknown_bank && f.at == cfg::Loc{0x4200, 2}) unknown_bank = true;
        else if (f.issue != cfg::Issue::computed_call) unexpected = true;
    }
    CHECK(unknown_bank && !unexpected);
}

} // namespace

int main() {
    test_sha1();
    test_rom_set();
    test_memory_map();
    test_bank_select();
    test_palette();
    test_slapstic();
    test_watchdog_and_interrupts();
    test_decoder();
    test_cfg();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all unit tests passed\n");
    return 0;
}
