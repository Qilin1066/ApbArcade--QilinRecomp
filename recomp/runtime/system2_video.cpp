#include "system2_video.hpp"

#include <algorithm>

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
}

void render(const Machine& machine, const Graphics& graphics, Frame& frame, int first, int last) {
    constexpr int W = Frame::width, H = Frame::height;
    first = std::max(first, 0);
    last = std::min(last, H - 1);
    if (first > last) return;
    const auto& pages = machine.video().pages();
    thread_local std::array<std::uint8_t, W * H> priority;
    thread_local std::array<std::uint16_t, W * H> mo;

    // Playfield: opaque, its category into the priority map.
    const int sx = machine.playfield_scroll_x(), sy = machine.playfield_scroll_y();
    const unsigned banks[2] = {machine.playfield_tile_bank(0), machine.playfield_tile_bank(1)};
    for (int y = first; y <= last; ++y) {
        const int ty = (y + sy) & 511;
        for (int x = 0; x < W; ++x) {
            const int tx = (x + sx) & 1023;
            const unsigned index = static_cast<unsigned>((ty / 8) * 128 + tx / 8);
            const std::uint16_t data = index < 0x1000 ? pages[1][index] : pages[2][index & 0xfff];
            const unsigned code = (banks[(data >> 10) & 1] << 10) | (data & 0x3ff);
            const unsigned color = (data >> 11) & 7;
            frame.pens[y * W + x] = static_cast<std::uint16_t>(128 + color * 16 + graphics.tile(code)[(ty & 7) * 8 + (tx & 7)]);
            priority[y * W + x] = static_cast<std::uint8_t>((~data >> 14) & 3);
        }
    }

    // Motion objects into their own bitmap (0xFFFF: nothing), as atarimo.cpp
    // does for APB's configuration: linked from entry 0, drawn in list order,
    // 16 pixels wide, a set neighbor bit placing one 16 pixels right of the
    // one before, positions wrapped against MAME's 640 x 416 screen bitmap.
    std::fill(mo.begin() + first * W, mo.begin() + (last + 1) * W, std::uint16_t{0xffff});
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
        for (int t = 0; t < height; ++t, ++code) {
            const int top = ypos + t * 16;
            if (top <= first - 16) continue;
            if (top > last) break;
            if (xpos <= -16 || xpos >= W) continue;
            const std::uint8_t* pixels = graphics.sprite(code);
            for (int py = std::max(0, first - top); py < 16 && top + py <= last; ++py) {
                const int y = top + py;
                for (int px = std::max(0, -xpos); px < 16 && xpos + px < W; ++px) {
                    const auto pen = pixels[py * 16 + (hflip ? 15 - px : px)];
                    if (pen != 15) mo[y * W + xpos + px] = static_cast<std::uint16_t>(pen + color);
                }
            }
        }
    }
    for (int i = first * W; i < (last + 1) * W; ++i) {
        if (mo[i] == 0xffff) continue;
        const int mo_priority = mo[i] >> 12;
        if (((mo_priority + priority[i]) & 2) != 0) {
            if (!(frame.pens[i] & 0x08)) frame.pens[i] = mo[i] & 0x0fff;   // high-priority playfield pens 8-15 win
        } else {
            frame.pens[i] = mo[i] & 0x0fff;
        }
    }

    // Alphanumerics on top, pen 0 transparent.
    for (int y = first; y <= last; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::uint16_t data = pages[0][(y / 8) * 64 + x / 8];
            const unsigned pen = graphics.character(data & 0x3ff)[(y & 7) * 8 + (x & 7)];
            if (pen != 0) frame.pens[y * W + x] = static_cast<std::uint16_t>(64 + ((data >> 13) & 7) * 4 + pen);
        }
    }
}

void Video::draw_through(const Machine& machine, int scanline) {
    scanline = std::min(scanline, static_cast<int>(Frame::height) - 1);
    if (scanline < next_line_) return;
    if (!only_.empty() && std::ranges::find(only_, frames_) == only_.end()) return;
    render(machine, graphics_, *drawing_, next_line_, scanline);
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

} // namespace qilin::sys2
