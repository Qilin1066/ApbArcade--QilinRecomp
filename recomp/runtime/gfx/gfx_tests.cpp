// Renderer tests: clears and presents on whatever Vulkan device there is,
// checked by reading the frames back. Exits 77 (skipped) without Vulkan.
#include <cstdio>
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
        return 77;
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
    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("all renderer tests passed\n");
    return failures != 0;
}
