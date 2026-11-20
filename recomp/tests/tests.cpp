// Unit tests: no game files needed.
//
// The board models are checked against MAME's own definitions, transcribed
// here independently of recomp/runtime/system2.hpp: MAME's bank table, its
// palette formula on every value, and its Slapstic 103-110 state machine on
// random bus traffic.

#include "cfg.hpp"
#include "controls.hpp"
#include "m6502_cfg.hpp"
#include "romset.hpp"
#include "system2.hpp"
#include "system2_machine.hpp"
#include "system2_audio.hpp"
#include "system2_video.hpp"
#include "t11_decoder.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <vector>

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

// A jump table guarded by a BIT mask test, APB's form at 0x8820, and a
// pointer immediate stored by window code into its own window.
void test_cfg_mask_table_and_window_pointer() {
    namespace cfg = qilin::cfg;
    Image image;
    image.put(0x8000, {
        013701, 0x0100,       // 8000 MOV @#100,R1       anything
        062701, 020,          // 8004 ADD #20,R1
        032701, 0177740,      // 8008 BIT #177740,R1     Z: R1 < 32
        001402,               // 800C BEQ 8012
        005000,               // 800E CLR R0
        000207,               // 8010 RTS PC
        006301,               // 8012 ASL R1
        000171, 0x9000,       // 8014 JMP @9000(R1)      32 entries, never 33
    });
    for (unsigned k = 0; k < 32; ++k) image.put(0x9000 + 2 * k, {static_cast<std::uint16_t>(0x8200 + 2 * k)});
    image.put(0x9040, {0x8300});   // past the end: never a target
    for (unsigned k = 0; k < 32; ++k) image.put(0x8200 + 2 * k, {000207});
    image.put(0x8300, {0});        // HALT: would be a finding if reached
    // Bank 12 (selected by writing 0) runs at 0x4000 and stores a pointer into itself.
    image.put(0x8100, {005037, 0x1400, 004737, 0x4000, 000207});   // CLR @#012000; JSR PC,@#4000; RTS PC
    const std::uint32_t bank12 = 0x10000 + 12 * 0x2000;
    image.put(bank12, {012737, 0x4100, 0x0200, 000207});           // MOV #4100,@#200; RTS PC
    image.put(bank12 + 0x100, {005200, 000207});                   // 4100: INC R0; RTS PC
    const auto set = image.set();
    const auto program = cfg::Program::analyze(set, {cfg::Loc{0x8100, 0}});
    const auto& table = program.nodes().at(cfg::Loc{0x8014, 0});
    CHECK(table.flow == cfg::Flow::table && table.table.size() == 32 && !table.heuristic_table);
    CHECK(program.nodes().contains(cfg::Loc{0x823E, 0}) && !program.nodes().contains(cfg::Loc{0x8300, 0}));
    CHECK(program.pointer_entries().contains(cfg::Loc{0x4100, 12}));
    CHECK(program.nodes().contains(cfg::Loc{0x4102, 12}));
}

// A byte index minus a constant, bounded by an unsigned compare: the range
// wraps past 0 before the CMP, and the BHI's fall-through still proves 24
// entries (APB's form at 5618 in bank 8).
void test_cfg_wrapped_index() {
    namespace cfg = qilin::cfg;
    Image image;
    image.put(0x8000, {
        005001,               // 8000 CLR R1
        0153701, 0x0100,      // 8002 BISB @#100,R1     R1 = 0..255
        010100,               // 8006 MOV R1,R0
        0162700, 024,         // 8008 SUB #24,R0        R0 = -20..235: wraps
        020027, 027,          // 800C CMP R0,#27
        0101003,              // 8010 BHI 8018
        006300,               // 8012 ASL R0
        066007, 0x9000,       // 8014 ADD 9000(R0),PC   (PC after = 8018)
        000207,               // 8018 RTS PC
    });
    for (unsigned k = 0; k < 24; ++k) image.put(0x9000 + 2 * k, {static_cast<std::uint16_t>(0x100 + 4 * k)});
    image.put(0x9030, {0x0001});   // past the end: odd, never a target
    for (unsigned k = 0; k < 24; ++k) image.put(0x8118 + 4 * k, {000240, 000207});
    const auto set = image.set();
    const auto program = cfg::Program::analyze(set);
    const auto& table = program.nodes().at(cfg::Loc{0x8014, 0});
    CHECK(table.flow == cfg::Flow::table && table.table.size() == 24 && !table.heuristic_table);
}

// A proven table outranks a guess: a function found only through a pointer
// in data holds a table whose first target, a three-word CMP, has a third
// word that a later data pointer would make the start of a NEG X(R0), hiding
// the real code after it (APB's table at D5BA and the data word D5D4).
void test_cfg_table_over_guess() {
    namespace cfg = qilin::cfg;
    Image image;
    image.put(0x8000, {000777});   // reset: BR .
    image.put(0x8100, {
        013700, 0x0100,       // 8100 MOV @#100,R0
        020027, 3,            // 8104 CMP R0,#3
        0101003,              // 8108 BHI 8110
        006300,               // 810A ASL R0
        066007, 0x9000,       // 810C ADD 9000(R0),PC   (PC after = 8110)
        000207,               // 8110 RTS PC
    });
    image.put(0x9000, {0x00F0, 0x0100, 0x0110, 0x0120});
    image.put(0x8200, {023737, 0x0200, 0x0B30, 000207, 000207});   // CMP @#200,@#5460; RTS; RTS
    for (const std::uint32_t at : {0x8210u, 0x8220u, 0x8230u}) image.put(at, {000207});
    image.put(0xA000, {0x8100, 0x8204});   // data: the function, and the misleading word
    const auto set = image.set();
    const auto program = cfg::Program::analyze(set);
    const auto table = program.nodes().find(cfg::Loc{0x810C, 0});
    CHECK(table != program.nodes().end() && table->second.flow == cfg::Flow::table && table->second.table.size() == 4 &&
          !table->second.heuristic_table);
    CHECK(program.nodes().contains(cfg::Loc{0x8200, 0}) && program.nodes().contains(cfg::Loc{0x8206, 0}));
    CHECK(!program.functions().contains(cfg::Loc{0x8204, 0}) && program.denied_entries().contains(cfg::Loc{0x8204, 0}));
}

// Code nothing reaches, after data in the same gap: a routine that ends
// exactly where known code begins is found (APB's at 9398), and tables of
// powers of two beside code, which decode as RTS R0, BR, BNE..., are not.
void test_cfg_routine_inside_gap() {
    namespace cfg = qilin::cfg;
    Image image;
    image.put(0x8000, {004737, 0x8026, 004737, 0x8034, 000777});   // JSR PC,@#8026; JSR PC,@#8034; BR .
    image.put(0x8010, {0x0080, 0x0100, 0x0200, 0x0400});           // a table, then zeros
    image.put(0x8020, {010100, 006300, 000207});                   // MOV R1,R0; ASL R0; RTS PC: nothing calls it
    image.put(0x8026, {005200, 000207});                           // INC R0; RTS PC
    image.put(0x8030, {0x0100, 0x0080});                           // a table right against code
    image.put(0x8034, {005300, 000207});                           // DEC R0; RTS PC
    const auto set = image.set();
    const auto program = cfg::Program::analyze(set);
    const auto& nodes = program.nodes();
    CHECK(nodes.contains(cfg::Loc{0x8026, 0}) && nodes.contains(cfg::Loc{0x8034, 0}));
    CHECK(program.gap_entries().contains(cfg::Loc{0x8020, 0}) && nodes.contains(cfg::Loc{0x8024, 0}));
    for (const std::uint16_t data : std::array<std::uint16_t, 6>{0x8010, 0x8012, 0x8014, 0x8016, 0x8030, 0x8032}) CHECK(!nodes.contains(cfg::Loc{data, 0}));
    for (const auto& f : program.findings()) CHECK(f.issue != cfg::Issue::overlap);
}

// The board model against MAME's atarisy2.cpp: the latches and the lines they
// drive, the delayed interrupt enable, the 32V and VBLANK interrupts, the
// sound IRQ (not while the 6502 is held in reset), the watchdog, sound-side
// mirrors and main-side byte lanes.
void test_machine() {
    std::vector<std::uint8_t> main_rom(0x90000, 0), sound_rom(0x10000, 0);
    sys2::Machine m(main_rom, sound_rom);
    CHECK(m.sound_held_in_reset());
    m.main_write16(0x15A0, 0);                 // sound_reset_w(0): the 6502 runs
    CHECK(!m.sound_held_in_reset());
    m.main_write8(0x1681, 0x12);               // the odd byte: not the latch's lane
    CHECK(!m.sound_nmi_line());

    // The interrupt enable takes effect at the next synchronisation.
    m.main_write16(0x1600, 0x000f);
    m.sound_write(0x1874, 0x56);               // reply: CP1 from the enable as it was (0)
    CHECK((m.main_cp_lines() & 2) == 0);
    CHECK((m.main_read16(0x1800) & 0x30) == 0x10 && (m.sound_read(0x1840) & 3) == 2);
    CHECK(m.main_read16(0x1C00) == 0xff56 && (m.main_read16(0x1800) & 0x10) == 0);
    m.run_events(0);                           // applies the enable; line 0's 32V interrupt
    CHECK((m.main_cp_lines() & 4) != 0);
    m.main_write16(0x15C0, 0);
    CHECK((m.main_cp_lines() & 4) == 0);

    // Command: NMI, IN0 bit 5 and IN1 bit 0 until the 6502 reads it, which clocks CP0.
    m.main_write16(0x1680, 0x1234);
    CHECK(m.sound_nmi_line() && (m.main_read16(0x1800) & 0x20) != 0 && (m.sound_read(0x1840) & 1) == 1);
    CHECK(m.sound_read(0x3C60) == 0x34);       // 0x1860 through mirror bits 10 and 13
    CHECK(!m.sound_nmi_line() && (m.main_cp_lines() & 1) != 0);
    m.main_write16(0x1580, 0);
    CHECK((m.main_cp_lines() & 1) == 0);
    m.sound_write(0x1874, 0x99);
    CHECK((m.main_cp_lines() & 2) != 0);       // CP1 now enabled
    m.main_read16(0x1C00);
    CHECK((m.main_cp_lines() & 2) == 0);

    // VBLANK at line 384: CP3 and a frame.
    m.main_time = 384 * sys2::cycles_per_scanline;
    m.run_events(m.main_time);
    CHECK((m.main_cp_lines() & 8) != 0 && m.frames() == 1);

    // The 244 Hz sound IRQ, acknowledged at 0x1878; none while the 6502 is held in reset.
    m.main_time = sys2::sound_irq_period;
    m.main_write16(0x1800, 0);                 // keep the watchdog quiet
    m.run_events(m.main_time);
    CHECK(m.sound_irq_line());
    m.sound_write(0x1879, 0);
    CHECK(!m.sound_irq_line());
    m.main_write16(0x15A0, 1);
    m.main_time = 2 * sys2::sound_irq_period;
    m.run_events(m.main_time);
    CHECK(!m.sound_irq_line());

    // The watchdog: a strobe each period keeps it quiet; then it runs out.
    m.main_write16(0x1800, 0);
    CHECK(!m.take_watchdog_expiry());
    m.main_time += sys2::watchdog_period;
    m.run_events(m.main_time);
    CHECK(m.take_watchdog_expiry() && !m.take_watchdog_expiry());

    // Bank select: writing 0 selects bank 12; sound RAM is mirrored at 0x2000.
    m.main_write16(0x1402, 0);
    CHECK(m.bank(1) == 12);
    m.sound_write(0x0123, 0x77);
    CHECK(m.sound_read(0x2123) == 0x77);
}

// The sound program's analysis on a synthetic 6502 ROM: a branch that is
// always taken in front of data, a command dispatcher like APB's (a class
// table and an argument table indexed by the same command, the class picking
// a handler through an RTS jump table, the RTS shared with a plain return), a
// handler dispatching again through the argument, and a callee whose
// argument range makes one of its branches always taken.
void test_sound_cfg() {
    namespace m6502 = qilin::m6502;
    std::vector<std::uint8_t> memory(0x10000, 0x02);   // KIL everywhere: any stray path shows
    const auto put = [&](unsigned address, std::initializer_list<std::uint8_t> bytes) {
        for (const auto b : bytes) memory[address++] = b;
    };
    put(0x4000, {
        0xA9, 0xFF,         // 4000 LDA #$FF
        0xD0, 0x03,         // 4002 BNE $4007 (always taken)
        0x02, 0x02, 0x02,   // 4004 data
        0xAC, 0x00, 0x02,   // 4007 LDY $0200      the command, from RAM
        0xC0, 0x08,         // 400A CPY #$08
        0xB0, 0x14,         // 400C BCS $4022      out of range: return
        0xB9, 0x00, 0x50,   // 400E LDA $5000,Y    its class
        0xC9, 0x02,         // 4011 CMP #$02
        0xB0, 0x0D,         // 4013 BCS $4022      no handler: return
        0x0A, 0xAA,         // 4015 ASL A; TAX
        0xBD, 0x11, 0x50,   // 4017 LDA $5011,X
        0x48,               // 401A PHA
        0xBD, 0x10, 0x50,   // 401B LDA $5010,X
        0x48,               // 401E PHA
        0xB9, 0x08, 0x50,   // 401F LDA $5008,Y    its argument
        0x60,               // 4022 RTS            to the handler, or back
    });
    put(0x5000, {0, 1, 0, 1, 5, 0, 1, 0});                          // classes of commands 0-7
    put(0x5008, {0x10, 0x20, 0x11, 0x21, 0x99, 0x12, 0x22, 0x13});  // arguments
    put(0x5010, {0xFF, 0x40, 0xFF, 0x41});                          // handlers 4100, 4200 (minus one)
    // Class 0: a second RTS table through the argument (0x10-0x13 here).
    put(0x4100, {0x0A, 0xA8, 0xB9, 0x01, 0x51, 0x48, 0xB9, 0x00, 0x51, 0x48, 0x60});
    put(0x5120, {0xFF, 0x42, 0x0F, 0x43, 0x1F, 0x43, 0x2F, 0x43});  // 4300, 4310, 4320, 4330
    put(0x5128, {0xFF, 0x43});                                      // a slot no command reaches: 4400
    for (const unsigned t : {0x4300u, 0x4310u, 0x4320u, 0x4330u}) put(t, {0x60});
    // Class 1: calls 4250 with A = 0x20-0x22; its BCC is always taken.
    put(0x4200, {0x20, 0x50, 0x42, 0x60});
    put(0x4250, {0xC9, 0x23, 0x90, 0x02, 0x02, 0x02, 0x60});
    put(0x4F00, {0x40});                                            // RTI
    put(0xFFFA, {0x00, 0x4F, 0x00, 0x40, 0x00, 0x4F});              // NMI, reset, IRQ

    const auto program = m6502::Program::analyze(memory);
    const auto& nodes = program.nodes();
    CHECK(!nodes.contains(0x4004) && nodes.at(0x4002).always_taken);
    const auto& dispatcher = nodes.at(0x4022);
    CHECK(dispatcher.transfer == m6502::Transfer::table && dispatcher.returns);
    CHECK((dispatcher.table == std::vector<std::uint16_t>{0x4100, 0x4200}));
    CHECK((nodes.at(0x410A).table == std::vector<std::uint16_t>{0x4300, 0x4310, 0x4320, 0x4330}));
    CHECK(!nodes.contains(0x4400) && !nodes.contains(0x4254));
    CHECK(nodes.at(0x4252).always_taken && program.functions().contains(0x4250));
    CHECK(program.findings().empty());
}

} // namespace

void test_video() {
    // MAME's layouts: bits most significant first, planes {0, 4} (and the
    // second half of the region for the 4-bit ones), x in nibbles a byte apart.
    std::vector<std::uint8_t> tiles(0x80000, 0), sprites(0x100000, 0), chars(0x4000, 0);
    tiles[0] = 0x88;            // tile 0, (0, 0): planes 0 and 1
    tiles[0x40000] = 0x80;      // and plane 2 from the second half: pen 14
    tiles[16 + 1] = 0x01;       // tile 1, (7, 0): plane 1 -> pen 4
    chars[16] = 0x80;           // char 1, (0, 0): plane 0 -> pen 2
    chars[2] = 0x08;            // char 0, (0, 1): plane 1 -> pen 1
    sprites[12 + 1] = 0x40;     // sprite 0, (5, 3): plane 0 -> pen 8
    const sys2::Graphics g(tiles, sprites, chars);
    CHECK(g.tile(0)[0] == 14 && g.tile(0)[1] == 0 && g.tile(1)[7] == 4);
    CHECK(g.character(1)[0] == 2 && g.character(0)[8] == 1);
    CHECK(g.sprite(0)[3 * 16 + 5] == 8);

    // RAM all zero: tile 0 everywhere in palette 128, alphanumeric 0 over it
    // (pen 1 at (0, 1) of every cell, palette 64), motion object 0 off the top.
    std::vector<std::uint8_t> main_rom(0x90000, 0), sound_rom(0x10000, 0);
    sys2::Machine m(main_rom, sound_rom);
    auto frame = std::make_unique<sys2::Frame>();
    sys2::render(m, g, *frame, 0, sys2::Frame::height - 1);
    CHECK(frame->pens[0] == 128 + 14 && frame->pens[1] == 128 && frame->pens[8] == 128 + 14);
    CHECK(frame->pens[sys2::Frame::width] == 64 + 1 && frame->pens[8 * sys2::Frame::width + 8] == 128 + 14);
    // Scrolled 3 pixels left: the tile's pen 14 column moves to x = 5.
    m.main_write16(0x1700, 3 << 6);
    sys2::render(m, g, *frame, 0, 0);
    CHECK(frame->pens[5] == 128 + 14 && frame->pens[0] == 128 && frame->pens[8 * sys2::Frame::width] == 128 + 14);

    // The two colour conversions agree, rotation included.
    frame->pens[3] = 200;
    m.main_write16(0x1000 + 200 * 2, 0xf84f);   // RRRRGGGGBBBBIIII: a bright red-orange
    m.main_write16(0x1000 + 128 * 2, 0x123c);
    CHECK(m.palette().color(200).red > m.palette().color(200).green && m.palette().color(128).blue > 0);
    const auto rgba = sys2::to_rgba(*frame, m.palette());
    std::vector<std::uint32_t> bgra(sys2::Frame::width * sys2::Frame::height);
    sys2::to_bgra(*frame, m.palette(), bgra.data());
    bool same = true;
    for (std::size_t i = 0; i < bgra.size(); ++i) {
        const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(&bgra[i]);
        same = same && p[0] == rgba[i * 4 + 2] && p[1] == rgba[i * 4 + 1] && p[2] == rgba[i * 4] && p[3] == 255;
    }
    CHECK(same);

    // Partial updates as MAME's screen makes them: a request from vblank
    // (line 384 on) draws nothing, the next frame starting at line 0. APB
    // links motion object 0 to the top half's list at line ~392 and to the
    // bottom half's at ~194; drawing the whole next frame at 392 showed it
    // with the list about to be replaced, and cars vanished.
    sys2::Video video(g);
    m.main_write16(0x1700, 0);
    (void)m.main_read16(0x8000);                 // the slapstic to bank 0: motion objects at 0x3800
    (void)m.main_read16(0x8080);
    CHECK(m.video().bank() == 0);
    m.main_write16(0x3808 + 0, static_cast<std::uint16_t>(396 << 6));   // object 1: sprite 0 at line 100,
    m.main_write16(0x3808 + 2, 0);
    m.main_write16(0x3808 + 4, static_cast<std::uint16_t>(50 << 6));    // x 50, linking back to 0
    m.main_write16(0x3808 + 6, 0);
    m.main_write16(0x3806, 1 << 3);              // object 0 links to it
    video.draw_through(m, 392);                  // in vblank: nothing drawn
    CHECK(video.bands() == 0);
    m.main_write16(0x3806, 0);                   // the list replaced before line 0: empty
    video.draw_through(m, 50);
    video.vblank(m);
    CHECK(video.bands() == 2 && video.frame().pens[103 * sys2::Frame::width + 55] == 128);
    video.draw_through(m, 120);                  // the object's list for lines 0-120
    m.main_write16(0x3806, 1 << 3);
    video.draw_through(m, 400);                  // vblank again: ignored
    video.vblank(m);
    CHECK(video.bands() == 4 && video.frame().pens[103 * sys2::Frame::width + 55] == 128);
    m.main_write16(0x3806, 1 << 3);
    video.vblank(m);
    CHECK(video.frame().pens[103 * sys2::Frame::width + 55] == 8);   // sprite 0's pen 8 at (5, 3)
}

void test_audio() {
    // A pure tone: channel 1, AUDF 99, the 64 kHz clock: the output flips
    // every 100 ticks of 28 cycles.
    sys2::PokeySound tone;
    tone.write(15, 0x03);   // SKCTL: out of reset
    tone.write(0, 99);
    tone.write(1, 0xaf);    // pure tone, volume 15
    for (int i = 0; i < 28 * 600; ++i) tone.step();   // past the counters' first wrap from power-on
    unsigned flips = 0, last = tone.output();
    for (int i = 0; i < 28 * 100 * 40; ++i) {
        tone.step();
        if (tone.output() != last) ++flips, last = tone.output();
    }
    CHECK(flips >= 39 && flips <= 41);

    // run(n) is n steps, whatever the registers: random writes at random times.
    sys2::PokeySound a, b;
    std::uint64_t x = 12345;
    auto next = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    bool same = true;
    for (int round = 0; round < 3000 && same; ++round) {
        const unsigned reg = static_cast<unsigned>(next() % 10);
        const auto value = static_cast<std::uint8_t>(next());
        const unsigned r = reg == 9 ? 15 : reg;   // AUDF/AUDC 1-4, AUDCTL, SKCTL
        std::uint8_t v = r == 15 ? static_cast<std::uint8_t>(value & 0x1b) : r == 8 ? static_cast<std::uint8_t>(value & (next() % 4 ? 0x9f : 0xff)) : value;
        if (r == 1) v |= 1;   // channel 1 always audible: a silent POKEY isn't run (checked below)
        a.write(r, v);
        b.write(r, v);
        const int n = static_cast<int>(next() % 3000);
        std::uint32_t stepped = 0;
        for (int i = 0; i < n; ++i) {
            a.step();
            stepped += std::min(a.output() * (32767u / 11 / 4), 32767u);
        }
        same = stepped == b.run(n) && a.output() == b.output();
    }
    CHECK(same);
    // Silent (every channel's volume 0, nothing forced): no output, for any length.
    sys2::PokeySound quiet;
    quiet.write(15, 0x07);
    quiet.write(8, 0x78);
    for (unsigned r = 0; r < 8; ++r) quiet.write(r, 0);
    CHECK(quiet.run(1 << 30) == 0 && quiet.output() == 0);

    // The YM2151 through the board: a key-on makes sound; the mixer latch scales it.
    sys2::Audio audio;
    std::int64_t t = 0;
    auto ym = [&](std::uint8_t reg, std::uint8_t value) {
        audio.ym_write(false, reg, t += 64);
        audio.ym_write(true, value, t += 64);
    };
    for (std::uint8_t op = 0; op < 4; ++op) {
        ym(static_cast<std::uint8_t>(0x60 + op * 8), op == 3 ? 0x00 : 0x7f);   // total level: only the carrier
        ym(static_cast<std::uint8_t>(0x80 + op * 8), 0x1f);                    // attack rate
        ym(static_cast<std::uint8_t>(0x40 + op * 8), 0x01);                    // multiple
        ym(static_cast<std::uint8_t>(0xe0 + op * 8), 0x0f);                    // release
    }
    ym(0x20, 0xc7);   // channel 0: both sides, algorithm 7
    ym(0x28, 0x4a);   // a note
    ym(0x08, 0x78);   // key on, all operators
    audio.advance(t += 32 * 2000);
    int peak = 0;
    for (const auto v : audio.frames()) peak = std::max(peak, std::abs(int{v}));
    CHECK(peak > 1000);
    audio.frames().clear();
    audio.mixer_write(0x00, t);   // every volume resistor in: quieter
    audio.advance(t += 32 * 2000);
    int quieter = 0;
    for (const auto v : audio.frames()) quieter = std::max(quieter, std::abs(int{v}));
    CHECK(quieter > 0 && quieter < peak);
}

void test_speech() {
    // APB's wiring: a byte into the latch, /WS low, wait for /READY, /WS
    // high. /READY goes inactive at /WS low and comes back 16 chip clocks
    // later (256 main cycles at the 625 kHz clock), when the chip takes the
    // byte; with the FIFO full, only once the chip has spoken some.
    sys2::Speech speech;
    int peak = 0;
    std::uint64_t made = 0;
    speech.on_sample = [&](std::int16_t v, std::int64_t) { peak = std::max(peak, std::abs(int{v})); ++made; };
    std::int64_t t = 1000;
    auto write = [&](std::uint8_t value) {   // returns the main cycles /READY was inactive
        speech.data_w(value, t);
        speech.wsq_w(0, t += 4);
        const std::int64_t from = t;
        while (speech.not_ready(t)) ++t;
        speech.wsq_w(1, t += 4);
        return t - 4 - from;
    };
    CHECK(!speech.not_ready(t));
    CHECK(write(0x60) == 256);   // speak external
    // Bytes go into the 16-byte FIFO in 16 clocks each; nine start the
    // speech; once it's full, a write waits for the chip to speak some.
    bool quick = true;
    for (int i = 0; i < 9; ++i) quick = quick && write(0x5a) == 256;
    CHECK(quick);
    std::int64_t longest = 0;
    for (int i = 0; i < 12; ++i) longest = std::max(longest, write(0x5a));
    CHECK(longest > 10'000);
    speech.advance(t + 2'000'000);   // 0.2 s of speech from the FIFO
    CHECK(made > 1000 && peak > 100);
}

// The wheel, the stick and the pedal arithmetic (recomp/runtime/app/controls.hpp).
void test_controls() {
    using namespace qilin::controls;
    // A 900-degree wheel at 160 counts a turn: a quarter turn right is 40 counts.
    Wheel wheel({900, 160, 1.0, 0, false});
    CHECK(wheel.update(0) == 0);                       // the first reading only sets where it is
    int total = 0;
    for (int i = 1; i <= 50; ++i) total += wheel.update(0.2 * i / 50);   // 0.2 of half of 900 = 90 degrees
    CHECK(total == 40);
    for (int i = 49; i >= 0; --i) total += wheel.update(0.2 * i / 50);
    CHECK(total == 0);                                 // back to where it started, nothing lost or gained
    // Slow turns aren't lost to rounding: 1000 tiny steps add up.
    Wheel slow({900, 160, 1.0, 0, false});
    slow.update(0);
    total = 0;
    for (int i = 1; i <= 1000; ++i) total += slow.update(0.2 * i / 1000);
    CHECK(total == 40);
    // Held at the lock, a stopped wheel keeps the dial turning; at the edge's start, not yet.
    Wheel lock({900, 160, 0.9, 6, false});
    lock.update(1.0);
    CHECK(lock.update(1.0) == 6);
    CHECK(lock.update(1.0) == 6);
    Wheel left({900, 160, 0.9, 6, false});
    left.update(-1.0);
    CHECK(left.update(-1.0) == -6);
    Wheel edge({900, 160, 0.9, 6, false});
    edge.update(0.9);
    CHECK(edge.update(0.9) == 0);
    Wheel inverted({900, 160, 1.0, 0, true});
    inverted.update(0);
    CHECK(inverted.update(0.1) < 0);
    // A continuous axis crossing its seam keeps the same direction, and
    // holding near that seam must never invoke the limited-wheel edge spin.
    Wheel continuous({360, 160, 0.9, 6, false, true});
    continuous.update(0.75);
    CHECK(continuous.update(-0.75) == 40);
    CHECK(continuous.update(-0.95) == -16);
    CHECK(continuous.update(-0.95) == 0);
    // Three complete turns between frames: all input events count even
    // though the last position is identical to the first.
    continuous.reset();
    continuous.update(0);
    for (int i = 1; i <= 48; ++i) {
        double axis = std::fmod(i / 8.0, 2.0);
        if (axis >= 1) axis -= 2;
        continuous.observe(axis);
    }
    CHECK(continuous.update(0) == 480);
    Wheel reversed_wrap({360, 160, 1.0, 0, true, true});
    reversed_wrap.update(0.75);
    CHECK(reversed_wrap.update(-0.75) == -40);
    // A G920 has real endpoints: a large move must not be mistaken for a seam.
    Wheel bounded({900, 160, 1.0, 0, false});
    bounded.update(0.9);
    CHECK(bounded.update(-0.9) == -360);
    // A physical-angle provider has no finite SDL axis range. Its first
    // reading/reconnection is a baseline, including a large turn count.
    Wheel physical;
    CHECK(physical.update_degrees(36000) == 0);
    CHECK(physical.update_degrees(36360) == 160);
    CHECK(physical.update_degrees(36000) == -160);
    CHECK(physical.update_degrees(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(physical.update_degrees(-72000) == 0);
    CHECK(physical.update_degrees(-71640) == 160);
    physical.reset();
    CHECK(physical.update_degrees(0) == 0);
    // Measuring one full turn works even when it ends on its starting
    // reading: signed travel includes the seam, rather than end minus start.
    AxisTravel measured;
    measured.reset(true);
    measured.observe(0);
    for (int i = 1; i <= 16; ++i) {
        double axis = std::fmod(i / 8.0, 2.0);
        if (axis >= 1) axis -= 2;
        measured.observe(axis);
    }
    CHECK(measured.travel() == 2 && 720 / measured.travel() == 360);
    // A stick: nothing inside the dead zone, the full rate (3) at full
    // deflection, and the curve: half way over is (0.35 / 0.85)^2 = 17% of it,
    // 5 counts in 10 frames. Straight (curve 1), 41%: 12. Keys: as asked.
    Rate stick;
    CHECK(stick.update(0.1) == 0);
    CHECK(stick.update(1.0) == 3);
    CHECK(stick.update(-1.0) == -3);
    int half = 0;
    for (int i = 0; i < 10; ++i) half += stick.update(0.5);
    CHECK(half == 5);
    Rate straight;
    straight.curve = 1;
    int straight_half = 0;
    for (int i = 0; i < 10; ++i) straight_half += straight.update(0.5);
    CHECK(straight_half == 12);
    Rate keys;
    int key_counts = 0;
    for (int i = 0; i < 10; ++i) key_counts += keys.update_linear(-0.6);
    CHECK(key_counts == -18);
    // Pedals: a reversed one (1 released, -1 floored), the dead zones, APB's inverted ADC.
    const PedalSettings reversed{1, -1, 0.04};
    CHECK(pedal_travel(1, reversed) == 0);
    CHECK(pedal_travel(-1, reversed) == 1);
    CHECK(pedal_travel(0.98, reversed) == 0);
    CHECK(std::abs(pedal_travel(0, reversed) - 0.5) < 1e-9);
    CHECK(pedal_adc(0) == 0x3f);
    CHECK(pedal_adc(1) == 0);
    const PedalSettings flat{0.5, 0.5, 0.04};
    CHECK(pedal_travel(0.7, flat) == 0);              // uncalibrated: no throttle, not a stuck one
}

int main() {
    test_sha1();
    test_controls();
    test_rom_set();
    test_memory_map();
    test_bank_select();
    test_palette();
    test_slapstic();
    test_watchdog_and_interrupts();
    test_decoder();
    test_cfg();
    test_cfg_mask_table_and_window_pointer();
    test_cfg_wrapped_index();
    test_cfg_table_over_guess();
    test_cfg_routine_inside_gap();
    test_machine();
    test_video();
    test_audio();
    test_speech();
    test_sound_cfg();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all unit tests passed\n");
    return 0;
}
