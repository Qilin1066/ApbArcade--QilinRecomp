// The renderer: Vulkan 1.3, and only Vulkan (docs/roadmap-and-design-targets.md).
//
// Shared with GBTGhoulies-Qilincomp, less depth and stencil: System 2 video
// is 2D (playfield, motion objects and alphanumerics over one palette).
// Frames are rendered into the renderer's own images, so a frame can be read
// back, saved or scaled whether or not there's a window; presenting to a
// window copies the finished frame to it. Nothing else in the runtime touches
// Vulkan.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qilin::gfx {

struct Config {
    uint32_t width = 512, height = 384;  // the frame buffers' size in pixels: System 2's visible area
    uint32_t back_buffers = 1;           // with the front buffer, one more image
    int gpu = -1;                        // index among the Vulkan devices; -1 picks the best one
    bool validation = false;             // Vulkan validation layers, if installed (also QILIN_VK_VALIDATION=1)
};

struct Rect {
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
};

struct Image {  // 8-bit RGBA, rows top to bottom
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

class Renderer {
public:
    // Creates the renderer on a Vulkan 1.3 device, or returns null with the
    // reason in *error: no Vulkan library, no device, or a missing feature.
    static std::unique_ptr<Renderer> create(const Config& config, std::string* error);
    virtual ~Renderer() = default;

    virtual const std::string& device_name() const = 0;  // "NVIDIA GeForce …", "llvmpipe (LLVM …)"
    virtual const Config& config() const = 0;

    // Clears the back buffer: all of it (count 0) or the given rectangles.
    // rgba is 0..1 per channel.
    virtual void clear(const Rect* rects, uint32_t count, const float rgba[4]) = 0;

    // Ends the frame: the back buffer becomes the front buffer (a flip), and
    // the oldest buffer becomes the new back buffer.
    virtual void present() = 0;
    virtual uint64_t frames_presented() const = 0;
    // The frame buffers are numbered 0 … back_buffers; these say which is which now.
    virtual uint32_t back_index() const = 0;
    virtual uint32_t front_index() const = 0;
    // Submits what's recorded and waits for the GPU to finish it.
    virtual void flush() = 0;

    // Reads a frame buffer back: the front buffer (the last frame presented)
    // or the back buffer as drawn so far.
    virtual Image read_front_buffer() = 0;
    virtual Image read_back_buffer() = 0;

    // Validation messages of error severity so far (0 without the layers).
    virtual uint32_t validation_errors() const = 0;
};

// Writes an image as a PNG (uncompressed). False if the file can't be written.
bool write_png(const std::string& path, const Image& image);

}  // namespace qilin::gfx
