// Renderer tests: clears and presents on whatever Vulkan device there is,
// checked by reading the frames back. Exits 77 (skipped) without Vulkan.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gfx.hpp"

using namespace qilin;

namespace {
int failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)

bool pixel_is(const gfx::Image& im, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t* p = &im.rgba[(size_t(y) * im.width + x) * 4];
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}
}  // namespace

int main(int argc, char** argv) {
    gfx::Config cfg;
    cfg.width = 64;
    cfg.height = 48;
    cfg.validation = true;  // if the layers are installed
    std::string error;
    auto r = gfx::Renderer::create(cfg, &error);
    if (!r) {
        std::printf("renderer tests skipped: %s\n", error.c_str());
        const char* required = std::getenv("QILIN_REQUIRE_VULKAN");
        return required && std::string(required) == "1" ? 1 : 77;
    }
    std::printf("device: %s\n", r->device_name().c_str());

    // Buffers start black.
    CHECK(pixel_is(r->read_back_buffer(), 0, 0, 0, 0, 0, 0));

    // A whole-buffer clear, then a rectangle (clipped at the edge) over it.
    const float blue[4] = {0, 0, 1, 1}, red[4] = {1, 0, 0, 1};
    r->clear(nullptr, 0, blue);
    const gfx::Rect rect{48, 40, 100, 100};
    r->clear(&rect, 1, red);
    gfx::Image back = r->read_back_buffer();
    CHECK(pixel_is(back, 0, 0, 0, 0, 255, 255));
    CHECK(pixel_is(back, 47, 39, 0, 0, 255, 255));
    CHECK(pixel_is(back, 48, 40, 255, 0, 0, 255));
    CHECK(pixel_is(back, 63, 47, 255, 0, 0, 255));

    // Present flips: the frame becomes the front buffer; the next back buffer is the other one.
    CHECK(r->back_index() == 0 && r->front_index() == 1);
    r->present();
    CHECK(r->frames_presented() == 1 && r->back_index() == 1 && r->front_index() == 0);
    gfx::Image front = r->read_front_buffer();
    CHECK(pixel_is(front, 0, 0, 0, 0, 255, 255) && pixel_is(front, 50, 45, 255, 0, 0, 255));
    CHECK(pixel_is(r->read_back_buffer(), 0, 0, 0, 0, 0, 0));
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1};
    r->clear(nullptr, 0, grey);
    r->present();
    front = r->read_front_buffer();
    CHECK(pixel_is(front, 10, 10, 128, 128, 128, 255) || pixel_is(front, 10, 10, 127, 127, 127, 255));

    // CPU pixels into the back buffer (B, G, R, A in memory), then a clear of part of it.
    std::vector<uint32_t> pixels(64 * 48);
    for (uint32_t i = 0; i < pixels.size(); ++i) pixels[i] = 0xff000000u | (i % 64) << 16 | (i / 64) << 8 | 0x33;
    r->write_back_buffer(pixels.data());
    const gfx::Rect corner{0, 0, 2, 2};
    r->clear(&corner, 1, red);
    back = r->read_back_buffer();
    CHECK(pixel_is(back, 10, 20, 10, 20, 0x33, 255) && pixel_is(back, 63, 47, 63, 47, 0x33, 255));
    CHECK(pixel_is(back, 1, 1, 255, 0, 0, 255) && pixel_is(back, 2, 2, 2, 2, 0x33, 255));
    r->present();
    CHECK(pixel_is(r->read_front_buffer(), 5, 6, 5, 6, 0x33, 255));

    if (argc > 1) CHECK(gfx::write_png(argv[1], front));
    CHECK(r->validation_errors() == 0);
    CHECK(r->read_output().rgba.empty());   // no window and no output size: nothing drawn

    // The window's picture without a window (Config::output_width): a 16 x 12
    // frame shown 4 times its size, centred in 80 x 56 with black borders,
    // plain and through each effect. Its left half is a staircase of dark
    // blue under light grey (an edge at 45 degrees, in steps of one pixel),
    // its right half flat mid grey.
    std::vector<uint32_t> art(16 * 12);
    for (uint32_t y = 0; y < 12; ++y)
        for (uint32_t x = 0; x < 16; ++x) art[y * 16 + x] = x >= 8 ? 0xff808080u : (x + y < 10 ? 0xffd0d0d0u : 0xff200040u);
    auto look = [&](gfx::Effect effect) {
        gfx::Config c;
        c.width = 16;
        c.height = 12;
        c.validation = true;
        c.output_width = 80;
        c.output_height = 56;
        c.effect = effect;
        c.upright_tube = true;
        std::string why;
        auto renderer = gfx::Renderer::create(c, &why);
        CHECK(renderer != nullptr);
        if (!renderer) return gfx::Image{};
        renderer->write_back_buffer(art.data());
        renderer->present();
        gfx::Image out = renderer->read_output();
        CHECK(renderer->validation_errors() == 0);
        return out;
    };
    auto at = [](const gfx::Image& im, uint32_t x, uint32_t y, int channel) { return int(im.rgba[(size_t(y) * im.width + x) * 4 + channel]); };
    const gfx::Image plain = look(gfx::Effect::none);
    CHECK(plain.width == 80 && plain.height == 56);
    if (plain.width == 80) {
        CHECK(pixel_is(plain, 0, 0, 0, 0, 0, 255) && pixel_is(plain, 79, 55, 0, 0, 0, 255));   // borders (x 8..71, y 4..51 is the frame)
        CHECK(pixel_is(plain, 8, 4, 0xd0, 0xd0, 0xd0, 255) && pixel_is(plain, 71, 51, 0x80, 0x80, 0x80, 255));
        CHECK(pixel_is(plain, 8 + 4 * 5 + 3, 4 + 4 * 4 + 3, 0xd0, 0xd0, 0xd0, 255));   // source (5,4): x+y 9, light; whole pixels, sharp
        CHECK(pixel_is(plain, 8 + 4 * 6, 4 + 4 * 4, 0x20, 0x00, 0x40, 255));          // source (6,4): dark
    }
    const gfx::Image smooth = look(gfx::Effect::pixel_art);
    if (smooth.width == 80) {
        // Flat areas are exact; the staircase's corners are cut along the 45-degree line.
        CHECK(pixel_is(smooth, 60, 30, 0x80, 0x80, 0x80, 255) && pixel_is(smooth, 9, 5, 0xd0, 0xd0, 0xd0, 255));
        CHECK(pixel_is(smooth, 8 + 4 * 6 + 1, 4 + 4 * 8 + 1, 0x20, 0x00, 0x40, 255)); // deep in the dark side: (6,8)
        // The light pixel (5,4)'s bottom-right corner now takes the dark colour, and the dark pixel (6,4)'s top-left the light.
        CHECK(at(smooth, 8 + 4 * 5 + 3, 4 + 4 * 4 + 3, 0) < 0x60);
        CHECK(at(smooth, 8 + 4 * 6, 4 + 4 * 4, 0) > 0x90);
    }
    const gfx::Image crt = look(gfx::Effect::crt);
    if (crt.width == 80) {
        CHECK(pixel_is(crt, 0, 0, 0, 0, 0, 255));
        // On the flat grey: lines (running down the frame) with darker gaps
        // between them, every 4 pixels across, and about the same brightness overall.
        int lo = 255, hi = 0, sum = 0;
        for (uint32_t x = 48; x < 72; ++x) {
            int column = 0;
            for (uint32_t y = 12; y < 48; ++y) column += at(crt, x, y, 0) + at(crt, x, y, 1) + at(crt, x, y, 2);
            column /= 36 * 3;
            lo = std::min(lo, column);
            hi = std::max(hi, column);
            sum += column;
        }
        CHECK(hi - lo > 20);
        CHECK(std::abs(sum / 24 - 0x80) < 40);
    }

    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("all renderer tests passed\n");
    return failures != 0;
}
