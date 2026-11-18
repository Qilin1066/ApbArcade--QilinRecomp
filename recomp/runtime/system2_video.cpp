#include "system2_video.hpp"

#include <algorithm>
#include <cstring>

namespace qilin::sys2 {

namespace {

// MAME reads graphics bits most significant first.
int bit(std::span<const std::uint8_t> region, std::uint32_t offset) {
    return (region[offset >> 3] >> (7 - (offset & 7))) & 1;
}

// A gfx_layout with planes {0, 4} and, for 4 bits, {half, half + 4}: x in
// groups of 4 bits, 8 bits apart; y every 'row_bits'; 'element_bits' each.
void decode(std::span<const std::uint8_t> region, unsigned size, bool four_planes, unsigned row_bits, unsigned element_bits,
            std::vector<std::uint8_t>& out, unsigned& count) {
    const std::uint32_t half = four_planes ? static_cast<std::uint32_t>(region.size()) * 8 / 2 : 0;
    count = static_cast<unsigned>((four_planes ? half : region.size() * 8) / element_bits);
    out.assign(static_cast<std::size_t>(count) * size * size, 0);
    for (unsigned code = 0; code < count; ++code) {
        for (unsigned y = 0; y < size; ++y) {
            for (unsigned x = 0; x < size; ++x) {
                const std::uint32_t at = code * element_bits + y * row_bits + (x / 4) * 8 + (x % 4);
                int value = (bit(region, at) << 1) | bit(region, at + 4);
                if (four_planes) value = (value << 2) | (bit(region, half + at) << 1) | bit(region, half + at + 4);
                out[(static_cast<std::size_t>(code) * size + y) * size + x] = static_cast<std::uint8_t>(value);
            }
        }
    }
}

} // namespace

Graphics::Graphics(const rom::RomSet& set)
    : Graphics(set.region(rom::Region::tiles), set.region(rom::Region::sprites), set.region(rom::Region::chars)) {}

Graphics::Graphics(std::span<const std::uint8_t> tiles, std::span<const std::uint8_t> sprites, std::span<const std::uint8_t> chars) {
    decode(tiles, 8, true, 16, 128, tiles_, tile_count_);             // pflayout
    decode(sprites, 16, true, 32, 512, sprites_, sprite_count_);      // molayout
    decode(chars, 8, false, 16, 128, chars_, char_count_);            // anlayout
    char_rows_.assign(char_count_, 0);
    for (unsigned c = 0; c < char_count_; ++c)
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x)
                if (chars_[c * 64 + y * 8 + x]) char_rows_[c] = static_cast<std::uint8_t>(char_rows_[c] | 1u << y);
}

// The renderer works a row at a time and touches only what's there: tiles
// a span of pixels at a time, alphanumeric rows that have a visible pixel,
// and in the motion-object bitmap only the spans objects were drawn to. The
// result is pixel for pixel what MAME's screen_update makes.
void render(const Machine& machine, const Graphics& graphics, Frame& frame, int first, int last) {
    constexpr int W = Frame::width, H = Frame::height;
    first = std::max(first, 0);
    last = std::min(last, H - 1);
    if (first > last) return;
    const auto& pages = machine.video().pages();
    const int sx = machine.playfield_scroll_x(), sy = machine.playfield_scroll_y();
    const unsigned banks[2] = {machine.playfield_tile_bank(0), machine.playfield_tile_bank(1)};
    auto playfield_data = [&](int tx, int ty) {
        const unsigned index = static_cast<unsigned>((ty >> 3) * 128 + (tx >> 3));
        return index < 0x1000 ? pages[1][index] : pages[2][index & 0xfff];
    };

    // Playfield: opaque, 8 pixels of a tile at a time.
    for (int y = first; y <= last; ++y) {
        const int ty = (y + sy) & 511;
        std::uint16_t* out = &frame.pens[static_cast<std::size_t>(y) * W];
        for (int x = 0; x < W;) {
            const int tx = (x + sx) & 1023;
            const std::uint16_t data = playfield_data(tx, ty);
            const unsigned code = (banks[(data >> 10) & 1] << 10) | (data & 0x3ff);
            const auto base = static_cast<std::uint16_t>(128 + ((data >> 11) & 7) * 16);
            const std::uint8_t* pixels = graphics.tile(code) + (ty & 7) * 8;
            const int from = tx & 7, n = std::min(8 - from, W - x);
            for (int i = 0; i < n; ++i) out[x + i] = static_cast<std::uint16_t>(base + pixels[from + i]);
            x += n;
        }
    }

    // Motion objects into their own bitmap (0xFFFF: nothing), as atarimo.cpp
    // does for APB's configuration: linked from entry 0, drawn in list order,
    // 16 pixels wide, a set neighbor bit placing one 16 pixels right of the
    // one before, positions wrapped against MAME's 640 x 416 screen bitmap.
    // The bitmap is all 0xFFFF between calls; spans drawn are noted per row.
    thread_local std::vector<std::uint16_t> mo(static_cast<std::size_t>(W) * H, 0xffff);
    std::array<std::int16_t, H> span_from, span_to;
    std::fill(span_from.begin() + first, span_from.begin() + last + 1, std::int16_t{W});
    std::fill(span_to.begin() + first, span_to.begin() + last + 1, std::int16_t{-1});
    constexpr int bitmap_width = 640, bitmap_height = 416;
    std::array<bool, 256> visited{};
    int last_xpos = 0;
    for (unsigned link = 0, n = 0; n < 256 && !visited[link]; ++n) {
        visited[link] = true;
        const std::uint16_t* e = &pages[0][0xc00 + link * 4];
        link = (e[3] >> 3) & 0xff;
        unsigned code = (e[1] & 0x7ff) | ((e[0] & 7u) << 11);
        const unsigned color = ((e[3] >> 12) & 3) * 16 | (static_cast<unsigned>((e[3] >> 14) & 3) << 12);
        int xpos = (e[2] >> 6) & 0x3ff;
        const int height = ((e[1] >> 11) & 7) + 1;
        const bool hflip = (e[1] & 0x4000) != 0;
        int ypos = -((e[0] >> 6) & 0x1ff) - height * 16;
        if (e[1] & 0x8000) xpos = last_xpos + 16;
        last_xpos = xpos;
        xpos &= 1023;
        ypos &= 511;
        if (xpos >= bitmap_width) xpos -= 1024;
        if (ypos >= bitmap_height) ypos -= 512;
        if (xpos <= -16 || xpos >= W) continue;
        const int px0 = std::max(0, -xpos), px1 = std::min(16, W - xpos);
        for (int t = 0; t < height; ++t, ++code) {
            const int top = ypos + t * 16;
            if (top <= first - 16) continue;
            if (top > last) break;
            const std::uint8_t* pixels = graphics.sprite(code);
            for (int py = std::max(0, first - top); py < 16 && top + py <= last; ++py) {
                const int y = top + py;
                std::uint16_t* row = &mo[static_cast<std::size_t>(y) * W + xpos];
                const std::uint8_t* src = pixels + py * 16;
                bool any = false;
                for (int px = px0; px < px1; ++px) {
                    const auto pen = src[hflip ? 15 - px : px];
                    if (pen != 15) {
                        row[px] = static_cast<std::uint16_t>(pen + color);
                        any = true;
                    }
                }
                if (any) {
                    span_from[y] = static_cast<std::int16_t>(std::min<int>(span_from[y], xpos + px0));
                    span_to[y] = static_cast<std::int16_t>(std::max<int>(span_to[y], xpos + px1 - 1));
                }
            }
        }
    }
    // MAME's merge: a motion object loses only to playfield pens 8-15 of a
    // tile whose priority category, with the object's, has bit 1 set.
    for (int y = first; y <= last; ++y) {
        if (span_to[y] < 0) continue;
        const int ty = (y + sy) & 511;
        std::uint16_t* row = &mo[static_cast<std::size_t>(y) * W];
        std::uint16_t* out = &frame.pens[static_cast<std::size_t>(y) * W];
        for (int x = span_from[y]; x <= span_to[y]; ++x) {
            const std::uint16_t m = row[x];
            if (m == 0xffff) continue;
            row[x] = 0xffff;
            const int category = (~playfield_data((x + sx) & 1023, ty) >> 14) & 3;
            if (((m >> 12) + category) & 2) {
                if (!(out[x] & 0x08)) out[x] = m & 0x0fff;
            } else {
                out[x] = m & 0x0fff;
            }
        }
    }

    // Alphanumerics on top, pen 0 transparent; rows with nothing visible skipped.
    for (int y = first; y <= last; ++y) {
        const std::uint16_t* cells = &pages[0][(y >> 3) * 64];
        std::uint16_t* out = &frame.pens[static_cast<std::size_t>(y) * W];
        const unsigned r = static_cast<unsigned>(y & 7);
        for (int c = 0; c < 64; ++c) {
            const std::uint16_t data = cells[c];
            if (!(graphics.character_rows(data & 0x3ffu) >> r & 1)) continue;
            const std::uint8_t* pixels = graphics.character(data & 0x3ffu) + r * 8;
            const auto base = static_cast<std::uint16_t>(64 + ((data >> 13) & 7) * 4);
            for (int i = 0; i < 8; ++i)
                if (pixels[i]) out[c * 8 + i] = static_cast<std::uint16_t>(base + pixels[i]);
        }
    }
}

void Video::draw_through(const Machine& machine, int scanline) {
    scanline = std::min(scanline, static_cast<int>(Frame::height) - 1);
    if (scanline < next_line_) return;
    if (!only_.empty() && std::ranges::find(only_, frames_) == only_.end()) return;
    render(machine, graphics_, *drawing_, next_line_, scanline);
    ++bands_;
    next_line_ = scanline + 1;
}

void Video::vblank(const Machine& machine) {
    draw_through(machine, Frame::height - 1);
    std::swap(drawing_, done_);
    next_line_ = 0;
    ++frames_;
}

std::vector<std::uint8_t> to_rgba(const Frame& frame, const PaletteRam& palette) {
    constexpr unsigned W = Frame::width, H = Frame::height;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(W) * H * 4);
    for (unsigned y = 0; y < H; ++y) {
        for (unsigned x = 0; x < W; ++x) {
            // ROT270: source (x, y) goes to (y, W - 1 - x) of an H-wide image.
            const auto c = palette.color(static_cast<std::uint8_t>(frame.pens[y * W + x]));
            auto* p = &out[((W - 1 - x) * H + y) * 4];
            p[0] = c.red;
            p[1] = c.green;
            p[2] = c.blue;
            p[3] = 255;
        }
    }
    return out;
}

void to_bgra(const Frame& frame, const PaletteRam& palette, std::uint32_t* out) {
    constexpr unsigned W = Frame::width, H = Frame::height;
    std::array<std::uint32_t, 256> lookup;
    for (unsigned i = 0; i < 256; ++i) {
        const auto c = palette.color(static_cast<std::uint8_t>(i));
        const std::array<std::uint8_t, 4> bytes{c.blue, c.green, c.red, 255};
        std::memcpy(&lookup[i], bytes.data(), 4);
    }
    // ROT270, as to_rgba: source row y becomes destination column y, read bottom to top.
    for (unsigned y = 0; y < H; ++y) {
        const std::uint16_t* row = &frame.pens[y * W];
        std::uint32_t* column = out + y + (W - 1) * H;
        for (unsigned x = 0; x < W; ++x, column -= H) *column = lookup[row[x] & 0xff];
    }
}

} // namespace qilin::sys2
