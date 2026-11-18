// apb: the game, recompiled. A window (SDL3), the board's frames drawn by the
// reference renderer and shown through Vulkan, and the cabinet's controls.
//
//   apb <rom-dir> [--scale N] [--no-vsync] [--gpu N] [--frames N]
//
// Controls (keyboard; a gamepad works too):
//   Left / Right       steering wheel          (left stick)
//   Up                 gas pedal               (right trigger)
//   Left Ctrl or Z     button 1                (A / south)
//   Left Alt or X      button 2                (B / east)
//   Space or C         button 3                (X / west)
//   5, 6               coin slots 1 and 2      (Back)
//   9                  service coin
//   F2                 the self-test switch (on/off)
//   Escape             quit
//
// The EEPROM (high scores, audits, settings) is kept in SDL's per-user
// preferences folder for "Qilin/apb". Everything else follows the board:
// one game frame per VBLANK, 166,400 main-CPU cycles (60.1 Hz), paced to
// the clock; a frame is drawn as MAME draws it and shown on the next present.
//
// The game stops with a message when it can't go on: the watchdog, or
// control reaching code the lifter never saw (which would be a bug).

#include "game.hpp"
#include "gfx.hpp"
#include "romset.hpp"
#include "system2_machine.hpp"
#include "system2_video.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace qilin;

struct Options {
    const char* rom_dir = nullptr;
    int scale = 0;   // 0: the largest whole number that fits the display
    int gpu = -1;
    bool vsync = true;
    std::uint64_t frames = 0;   // 0: until closed
};

bool parse(int argc, char** argv, Options& o) {
    if (argc < 2) return false;
    o.rom_dir = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string_view flag = argv[i];
        auto number = [&](auto& out) {
            if (i + 1 >= argc) return false;
            const std::string_view v = argv[++i];
            return std::from_chars(v.data(), v.data() + v.size(), out).ec == std::errc{};
        };
        if (flag == "--no-vsync") o.vsync = false;
        else if (flag == "--scale") { if (!number(o.scale)) return false; }
        else if (flag == "--gpu") { if (!number(o.gpu)) return false; }
        else if (flag == "--frames") { if (!number(o.frames)) return false; }
        else return false;
    }
    return true;
}

[[noreturn]] void fail(const std::string& message) {
    std::print(stderr, "apb: {}\n", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "APB", message.c_str(), nullptr);
    SDL_Quit();
    std::exit(1);
}

std::string eeprom_path() {
    char* folder = SDL_GetPrefPath("Qilin", "apb");
    if (!folder) return {};
    std::string path = std::string(folder) + "eeprom.bin";
    SDL_free(folder);
    return path;
}

// The cabinet's controls from the keyboard and the first gamepad, once a frame.
class Controls {
public:
    // Applies the controls to the board's inputs; false once the player quits.
    bool update(sys2::Machine& m, std::uint64_t frame) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: return false;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) return false;
                if (e.key.key == SDLK_F2 && !e.key.repeat) test_switch_ = !test_switch_;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad_) pad_ = SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad_ && SDL_GetGamepadID(pad_) == e.gdevice.which) {
                    SDL_CloseGamepad(pad_);
                    pad_ = nullptr;
                }
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (renderer) renderer->resize_window(static_cast<std::uint32_t>(e.window.data1), static_cast<std::uint32_t>(e.window.data2));
                break;
            default: break;
            }
        }
        const bool* key = SDL_GetKeyboardState(nullptr);
        auto pad = [&](SDL_GamepadButton b) { return pad_ && SDL_GetGamepadButton(pad_, b); };

        // IN0, active low: MAME's apb buttons 1-3, the self-test switch.
        unsigned in0 = 0xffff;
        if (key[SDL_SCANCODE_LCTRL] || key[SDL_SCANCODE_Z] || pad(SDL_GAMEPAD_BUTTON_SOUTH)) in0 &= ~0x0080u;
        if (key[SDL_SCANCODE_LALT] || key[SDL_SCANCODE_X] || pad(SDL_GAMEPAD_BUTTON_EAST)) in0 &= ~0x0002u;
        if (key[SDL_SCANCODE_SPACE] || key[SDL_SCANCODE_C] || pad(SDL_GAMEPAD_BUTTON_WEST)) in0 &= ~0x0008u;
        if (test_switch_) in0 &= ~0x8000u;
        m.inputs.in0 = static_cast<std::uint16_t>(in0);
        // IN1, active low: coins and the service coin (the low bits are the board's own).
        unsigned in1 = 0xf0;
        if (key[SDL_SCANCODE_5] || pad(SDL_GAMEPAD_BUTTON_BACK)) in1 &= ~0x40u;
        if (key[SDL_SCANCODE_6]) in1 &= ~0x80u;
        if (key[SDL_SCANCODE_9]) in1 &= ~0x20u;
        m.inputs.in1 = static_cast<std::uint8_t>(in1);

        // The gas pedal, ADC1: 0x3F released, 0 floored (MAME's PORT_INVERT);
        // keys move it 8 a frame, as a foot would, the trigger sets it.
        int target = key[SDL_SCANCODE_UP] ? 0x3f : 0;
        if (pad_) target = std::max(target, SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 0x3f / 32767);
        pedal_ = target > pedal_ ? std::min(target, pedal_ + 8) : std::max(target, pedal_ - 8);
        m.inputs.adc[1] = static_cast<std::uint8_t>(0x3f - pedal_);

        // The steering wheel, a dial on LETA0: counts a frame.
        int turn = (key[SDL_SCANCODE_RIGHT] ? 3 : 0) - (key[SDL_SCANCODE_LEFT] ? 3 : 0);
        if (pad_) {
            const int x = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX);
            if (std::abs(x) > 6000) turn += x * 6 / 32767;
        }
        m.inputs.leta[0] = static_cast<std::uint8_t>(m.inputs.leta[0] + turn);
        (void)frame;
        return true;
    }

    gfx::Renderer* renderer = nullptr;

private:
    SDL_Gamepad* pad_ = nullptr;
    bool test_switch_ = false;
    int pedal_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        std::print(stderr, "usage: apb <rom-dir> [--scale N] [--no-vsync] [--gpu N] [--frames N]\n");
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::print(stderr, "apb: SDL: {}\n", SDL_GetError());
        return 1;
    }
    const auto set = rom::RomSet::load(options.rom_dir);
    if (!set) fail(set.error());

    // The window: the cabinet's monitor turned upright, scaled by a whole number.
    constexpr int width = sys2::Frame::height, height = sys2::Frame::width;   // 384 x 512 after ROT270
    int scale = options.scale;
    if (scale <= 0) {
        SDL_Rect usable{};
        scale = 1;
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) scale = std::max(1, std::min(usable.w / width, (usable.h - 64) / height));
    }
    SDL_Window* window = SDL_CreateWindow("APB", width * scale, height * scale, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) fail(std::string("can't open a window: ") + SDL_GetError());

    gfx::Config config;
    config.width = width;
    config.height = height;
    config.gpu = options.gpu;
    config.vsync = options.vsync;
    Uint32 count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!extensions) fail(std::string("SDL has no Vulkan: ") + SDL_GetError());
    for (Uint32 i = 0; i < count; ++i) config.instance_extensions.emplace_back(extensions[i]);
    config.create_surface = [window](void* instance) -> std::uint64_t {
        VkSurfaceKHR surface{};
        if (!SDL_Vulkan_CreateSurface(window, static_cast<VkInstance>(instance), nullptr, &surface)) return 0;
        return reinterpret_cast<std::uint64_t>(surface);
    };
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    config.window_width = static_cast<std::uint32_t>(pw);
    config.window_height = static_cast<std::uint32_t>(ph);
    std::string error;
    auto renderer = gfx::Renderer::create(config, &error);
    if (!renderer) fail(error);
    SDL_SetWindowTitle(window, std::format("APB  ({})", renderer->device_name()).c_str());

    // The board, its saved EEPROM, the video and the scheduler.
    sys2::Machine machine(set->region(rom::Region::main_cpu), set->region(rom::Region::sound_cpu));
    const std::string nv = eeprom_path();
    if (std::ifstream in{nv, std::ios::binary}; in) in.read(reinterpret_cast<char*>(machine.eeprom().data()), std::ssize(machine.eeprom()));
    const sys2::Graphics graphics(*set);
    sys2::Video video(graphics);
    machine.video_sink = &video;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    Controls controls;
    controls.renderer = renderer.get();

    game::Scheduler scheduler(machine);
    const double frame_ns = 166400.0 / 10e6 * 1e9;
    std::uint64_t start = SDL_GetTicksNS(), paced_from = 0;
    bool quit = false;
    scheduler.on_frame = [&](std::uint64_t frame) {
        if (!controls.update(machine, frame)) {
            quit = true;
            throw game::Stop{game::Stop::frames, "closed"};
        }
        sys2::to_bgra(video.frame(), machine.palette(), pixels.data());
        renderer->write_back_buffer(pixels.data());
        // Pace to the board's clock: frame n is due n frame-times after the
        // start. Falling more than a few frames behind restarts the count,
        // rather than racing to catch up.
        const auto due = start + static_cast<std::uint64_t>(static_cast<double>(frame - paced_from) * frame_ns);
        const auto now = SDL_GetTicksNS();
        if (now < due) SDL_DelayPrecise(due - now);
        else if (now - due > 5 * static_cast<std::uint64_t>(frame_ns)) {
            start = now;
            paced_from = frame;
        }
        renderer->present();
    };
    const auto stop = scheduler.run(options.frames ? options.frames : ~0ull);

    if (std::ofstream out{nv, std::ios::binary}; out) out.write(reinterpret_cast<const char*>(machine.eeprom().data()), std::ssize(machine.eeprom()));
    renderer.reset();
    SDL_DestroyWindow(window);
    if (!quit && stop.reason != game::Stop::frames) {
        const char* reasons[] = {"the frame count", "the watchdog", "unlifted main-CPU code", "unlifted sound-CPU code"};
        fail(std::format("the game stopped at frame {}: {}{}{}", machine.frames(), reasons[stop.reason], stop.detail.empty() ? "" : ": ", stop.detail));
    }
    SDL_Quit();
    return 0;
}
