// The renderer: Vulkan 1.3, and only Vulkan (docs/roadmap-and-design-targets.md).
//
// Shared with GBTGhoulies--QilinRecomp, less depth and stencil: System 2 video
// is 2D (playfield, motion objects and alphanumerics over one palette).
// Frames are rendered into the renderer's own images, so a frame can be read
// back, saved or scaled whether or not there's a window; presenting to a
// window copies the finished frame to it. Nothing else in the runtime touches
// Vulkan.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qilin::gfx {

// How a frame fills the window: the largest whole multiple of its size that
// fits (pixel-exact; below 1x, the largest fit), the largest fit with its
// shape kept, or the whole window.
enum class Scaling : uint8_t { integer, fit, stretch };

// A shader the window's picture is drawn through, or none (a plain blit, the
// cheapest): pixel-art smoothing (edges between colours drawn as lines, not
// steps) or a CRT (scanlines and phosphor stripes).
enum class Effect : uint8_t { none, pixel_art, crt };

struct Config {
    uint32_t width = 512, height = 384;  // the frame buffers' size in pixels: System 2's visible area
    uint32_t back_buffers = 1;           // with the front buffer, one more image
    int gpu = -1;                        // index among the Vulkan devices; -1 picks the best one
    std::string gpu_name;                // or the device with this name, if it's there (else the best one)
    bool validation = false;             // Vulkan validation layers, if installed (also QILIN_VK_VALIDATION=1)

    // A window to present to (optional). The windowing library gives the
    // instance extensions its surfaces need and makes the surface: called
    // with the VkInstance, it returns a VkSurfaceKHR, or 0 if it can't.
    std::vector<std::string> instance_extensions;
    std::function<uint64_t(void* instance)> create_surface;
    uint32_t window_width = 0, window_height = 0;  // the window's size in pixels
    // vsync: no tearing. Then either the newest finished frame is shown at
    // each refresh (mailbox, where offered, else FIFO): the caller paces
    // itself and no queue builds up; or, locked to the display, every frame
    // is shown in turn with the shortest queue (FIFO, 2 images), so presenting
    // waits for the display and paces the caller. Without vsync: mailbox or
    // immediate where offered.
    bool vsync = true;
    bool lock_to_display = false;
    Scaling scaling = Scaling::integer;
    bool smooth = true;                            // bilinear when the scale isn't whole; else nearest pixel
    Effect effect = Effect::none;
    float scanlines = 0.75f;                       // CRT: how dark the gaps between scanlines are, 0..1
    float mask = 0.3f;                             // CRT: the phosphor stripes' strength, 0..1
    bool upright_tube = false;                     // CRT: the frame was shown on a monitor turned upright (scanlines run down it)
    // With no window: present also draws the picture a window this size
    // would show (scaling, effect), for read_output. 0: not drawn.
    uint32_t output_width = 0, output_height = 0;
};

// What a UI drawn over the window (the settings menu) needs to draw with
// Vulkan, as plain handles so this header stays free of Vulkan's.
struct Overlay {
    void* instance = nullptr;         // VkInstance
    void* physical_device = nullptr;  // VkPhysicalDevice
    void* device = nullptr;           // VkDevice
    uint32_t queue_family = 0;
    void* queue = nullptr;            // VkQueue
    uint32_t format = 0;              // the window images' VkFormat
    uint32_t min_images = 2, images = 2;
};

struct Rect {
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
};

struct Image {  // 8-bit RGBA, rows top to bottom
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// A Vulkan failure the renderer can't go on from (the device lost: a driver
// reset, a GPU removed; or the window's surface lost) ends the program. The
// handler, if set, runs first, on the thread that was presenting: to save what
// must be kept and tell the player why.
void set_fatal_handler(std::function<void(const std::string& message)> handler);

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

    // Replaces the whole back buffer with pixels from the CPU: width x height
    // words, rows top to bottom, each B, G, R, A in memory order.
    virtual void write_back_buffer(const uint32_t* bgra) = 0;

    // Ends the frame: the back buffer becomes the front buffer (a flip), and
    // the oldest buffer becomes the new back buffer. With a window, the frame
    // is also shown there, scaled to fit with its aspect ratio kept (by a
    // whole number where it fits), centred on black.
    virtual void present() = 0;
    // The window's size in pixels changed (the swapchain is rebuilt at the next present).
    virtual void resize_window(uint32_t width, uint32_t height) = 0;
    // How frames fill the window, from the next present.
    virtual void set_scaling(Scaling scaling, bool smooth) = 0;
    // The shader the picture is drawn through, from the next present (scanlines and mask: the CRT's, 0..1).
    virtual void set_effect(Effect effect, float scanlines, float mask) = 0;
    // Waits for the display's refresh or not, and locked to it or not (Config::vsync; the swapchain is rebuilt at the next present).
    virtual void set_vsync(bool vsync) = 0;
    virtual void set_lock_to_display(bool lock) = 0;
    // Shows the last presented frame again (for a display refreshing at a multiple of the frame rate).
    virtual void present_again() = 0;
    // Whether the device can scale smoothly (a linear blit from the frame format); if not, frames are scaled sharp.
    virtual bool smooth_supported() const = 0;
    // The names of the Vulkan devices that can run the renderer, best first.
    virtual const std::vector<std::string>& devices() const = 0;
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
    // With no window and Config::output_width/height: the picture the last
    // present drew, as a window would show it. Empty otherwise.
    virtual Image read_output() = 0;

    // A UI over the window: the handles it draws with (false without a
    // window), and the drawing, called with the VkCommandBuffer (as void*)
    // inside a dynamic-rendering pass on the window's image after the frame
    // is shown there, every present. An empty function stops it.
    virtual bool overlay(Overlay& out) const = 0;
    virtual void set_overlay(std::function<void(void* command_buffer)> draw) = 0;

    // Validation messages of error severity so far (0 without the layers).
    virtual uint32_t validation_errors() const = 0;
};

// Writes an image as a PNG (uncompressed). False if the file can't be written.
bool write_png(const std::string& path, const Image& image);

}  // namespace qilin::gfx
