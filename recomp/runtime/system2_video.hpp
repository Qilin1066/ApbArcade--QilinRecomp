#pragma once
// System 2 video, rendered from the board's RAM as MAME's atarisy2_v.cpp and
// atarimo.cpp draw it: the 128 x 64 playfield of 8 x 8 tiles (4 bits a
// pixel, palette 128-255, scrolled, tile banks from the scroll registers,
// a priority category per tile), the motion objects (16 x 16 tiles, palette
// 0-63, a linked list from entry 0, later entries on top, pen 15
// transparent, each 1 tile wide and 1-8 high), merged by MAME's priority
// rule, and the 64 x 48 alphanumerics on top (2 bits, palette 64-95, pen 0
// transparent).
//
// This is the CPU reference: one frame at a time, into palette indices. The
// game will draw through the Vulkan renderer; this defines what it must match.

#include "romset.hpp"
#include "system2_machine.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace qilin::sys2 {

// The graphics ROMs decoded to one byte a pixel, with MAME's layouts.
class Graphics {
public:
    explicit Graphics(const rom::RomSet& set);
    // The regions as RomSet lays them out: tiles, sprites, alphanumerics.
    Graphics(std::span<const std::uint8_t> tiles, std::span<const std::uint8_t> sprites, std::span<const std::uint8_t> chars);

    [[nodiscard]] const std::uint8_t* tile(unsigned code) const noexcept { return &tiles_[(code % tile_count_) * 64]; }
    [[nodiscard]] const std::uint8_t* sprite(unsigned code) const noexcept { return &sprites_[(code % sprite_count_) * 256]; }
    [[nodiscard]] const std::uint8_t* character(unsigned code) const noexcept { return &chars_[(code % char_count_) * 64]; }
    // Bit r set: row r of the character has a visible pixel (pen 0 is transparent).
    [[nodiscard]] std::uint8_t character_rows(unsigned code) const noexcept { return char_rows_[code % char_count_]; }

private:
    std::vector<std::uint8_t> tiles_, sprites_, chars_, char_rows_;
    unsigned tile_count_ = 0, sprite_count_ = 0, char_count_ = 0;
};

// One visible frame, 512 x 384 palette indices, as the board scans it out
// (the cabinet rotates the monitor: see to_rgba).
struct Frame {
    static constexpr unsigned width = visible_width, height = visible_height;
    std::array<std::uint16_t, width * height> pens{};
};

// Draws scanlines [first, last] of a frame from the board as it is now.
void render(const Machine& machine, const Graphics& graphics, Frame& frame, int first, int last);

// Draws frames the way MAME's screen does: in bands, each ending where the
// game changes a scroll register or motion object 0's link, the rest at the
// start of vblank. Attach it as the machine's video sink.
class Video final : public Machine::VideoSink {
public:
    explicit Video(const Graphics& graphics) : graphics_(graphics) {}

    void draw_through(const Machine& machine, int scanline) override;
    void vblank(const Machine& machine) override;

    // Draws only these frames (counted from 0), when it's not every frame
    // that's wanted: drawing costs about a millisecond a frame on a desktop.
    void draw_only(std::vector<std::uint64_t> frames) { only_ = std::move(frames); }

    // The last frame completed, and how many have been.
    [[nodiscard]] const Frame& frame() const noexcept { return *done_; }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t bands() const noexcept { return bands_; }   // drawn so far

private:
    const Graphics& graphics_;
    std::unique_ptr<Frame> drawing_ = std::make_unique<Frame>(), done_ = std::make_unique<Frame>();
    int next_line_ = 0;
    std::uint64_t frames_ = 0, bands_ = 0;
    std::vector<std::uint64_t> only_;
};

// RGBA through the palette, rotated as the cabinet shows it (MAME's ROT270):
// 384 wide, 512 high.
std::vector<std::uint8_t> to_rgba(const Frame& frame, const PaletteRam& palette);
// The same into 32-bit words, B, G, R, A in memory: what the renderer takes.
// 'out' holds Frame::width * Frame::height words.
void to_bgra(const Frame& frame, const PaletteRam& palette, std::uint32_t* out);

} // namespace qilin::sys2
