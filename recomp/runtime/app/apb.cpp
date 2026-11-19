// apb: the game, recompiled. A window (SDL3), the board's frames drawn by the
// reference renderer and shown through Vulkan, and the cabinet's controls.
//
//   apb <rom-dir> [--scale N] [--no-vsync] [--gpu N] [--frames N] [--mute]
//
// Controls (keyboard; gamepads, wheels, pedals and mice too):
//   Left / Right       steering wheel          (left stick; a wheel; a mouse or spinner)
//   Up                 gas pedal               (right trigger; a pedal)
//   Left Ctrl or Z     button 1                (A / south)
//   Left Alt or X      button 2                (B / east)
//   Space or C         button 3                (X / west)
//   5, 6               coin slots 1 and 2      (Back)
//   9                  service coin
//   F2                 the self-test switch (on/off)
//   F1 or Escape       the settings menu       (Start or Guide)
//   Alt+Enter or F11   full screen (on/off)
// The menu sets up wheels (any: a direct-drive wheel that spins freely, as
// the cabinet's did, or one that stops at a lock), pedals and extra button
// bindings, and the display: full screen, which display, the picture's size
// (whole multiples, fit or fill; sharp or smooth), its look (plain, smoothed
// pixel art, or a CRT), vsync and the graphics device. They're kept in apb.ini next to the EEPROM; --no-vsync and --gpu N
// override the saved ones for one run.
//
// The EEPROM (high scores, audits, settings) is kept in SDL's per-user
// preferences folder for "Qilin/apb". Everything else follows the board:
// one game frame per VBLANK, 166,400 main-CPU cycles (60.1 Hz), paced to
// the clock; a frame is drawn as MAME draws it and shown on the next present.
// Sound (the YM2151, the POKEYs and the TMS5220's speech) is made as the 6502
// writes it and queued to SDL at 55,930 Hz, SDL resampling for the device.
// The queue is kept between one and four frames ahead.
//
// The game stops with a message when it can't go on: the watchdog, or
// control reaching code the lifter never saw (which would be a bug).

#include "game.hpp"
#include "gfx.hpp"
#include "input.hpp"
#include "text.hpp"
#include "menu.hpp"
#include "romset.hpp"
#include "system2_audio.hpp"
#include "system2_machine.hpp"
#include "system2_video.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
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
    bool mute = false;
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
        else if (flag == "--mute") o.mute = true;
        else if (flag == "--scale") { if (!number(o.scale)) return false; }
        else if (flag == "--gpu") { if (!number(o.gpu)) return false; }
        else if (flag == "--frames") { if (!number(o.frames)) return false; }
        else return false;
    }
    return true;
}

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "apb: %s\n", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "APB", message.c_str(), nullptr);
    SDL_Quit();
    std::exit(1);
}

// A file in SDL's per-user preferences folder for "Qilin/apb".
std::string pref_path(const char* file) {
    char* folder = SDL_GetPrefPath("Qilin", "apb");
    if (!folder) return file;
    std::string path = std::string(folder) + file;
    SDL_free(folder);
    return path;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        std::fprintf(stderr, "usage: apb <rom-dir> [--scale N] [--no-vsync] [--gpu N] [--frames N] [--mute]\n");
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "apb: SDL: %s\n", SDL_GetError());
        return 1;
    }
    const auto set = rom::RomSet::load(options.rom_dir);
    if (!set) fail(set.error());

    // The player's settings: the controls (for the input below) and the display.
    const std::string settings_path = pref_path("apb.ini");
    app::Settings saved;
    saved.load(settings_path);

    // The window: the cabinet's monitor turned upright, scaled by a whole
    // number, on the display it was last on; or full screen there.
    constexpr int width = sys2::Frame::height, height = sys2::Frame::width;   // 384 x 512 after ROT270
    SDL_DisplayID display = app::display_named(saved.display.monitor);
    if (!display) display = SDL_GetPrimaryDisplay();
    int scale = options.scale;
    if (scale <= 0) {
        SDL_Rect usable{};
        scale = 1;
        if (SDL_GetDisplayUsableBounds(display, &usable)) scale = std::max(1, std::min(usable.w / width, (usable.h - 64) / height));
    }
    SDL_Window* window = SDL_CreateWindow("APB", width * scale, height * scale,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window) fail(std::string("can't open a window: ") + SDL_GetError());
    SDL_SetWindowPosition(window, static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(display)), static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(display)));
    if (saved.display.fullscreen && !SDL_SetWindowFullscreen(window, true)) std::fprintf(stderr, "apb: full screen: %s\n", SDL_GetError());
    SDL_ShowWindow(window);
    SDL_SyncWindow(window);

    gfx::Config config;
    config.width = width;
    config.height = height;
    config.gpu = options.gpu;                     // --gpu N wins over the saved device
    if (options.gpu < 0) config.gpu_name = saved.display.gpu;
    config.vsync = options.vsync && saved.display.vsync;
    config.scaling = saved.display.scaling;
    config.smooth = saved.display.smooth;
    config.effect = saved.display.effect;
    config.scanlines = saved.display.scanlines;
    config.mask = saved.display.mask;
    config.upright_tube = true;   // the cabinet's monitor is turned upright (ROT270)
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
    SDL_SetWindowTitle(window, app::strf("APB  (%s)", renderer->device_name().c_str()).c_str());

    // The board, its saved EEPROM, the video and the scheduler.
    sys2::Machine machine(set->region(rom::Region::main_cpu), set->region(rom::Region::sound_cpu));
    const std::string nv = pref_path("eeprom.bin");
    if (std::ifstream in{nv, std::ios::binary}; in) in.read(reinterpret_cast<char*>(machine.eeprom().data()), std::ssize(machine.eeprom()));
    const sys2::Graphics graphics(*set);
    sys2::Video video(graphics);
    machine.video_sink = &video;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);

    // Sound: the board's chips, queued to the default playback device. No
    // device is not an error; the game runs silent, with a note.
    sys2::Audio audio;
    SDL_AudioStream* sound = nullptr;
    if (!options.mute) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 55930};
        sound = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (sound) {
            machine.sound_sink = &audio;
            SDL_ResumeAudioStreamDevice(sound);
        } else {
            std::fprintf(stderr, "apb: no sound: %s\n", SDL_GetError());
        }
    }
    constexpr int frame_bytes = 55930 / 60 * 4;   // one video frame of sound

    // The controls and the settings menu, with the player's saved settings.
    // (Pointers, so they go before the renderer and SDL do.)
    auto input_owner = std::make_unique<app::Input>();
    app::Input& input = *input_owner;
    input.settings = saved;
    input.configure();
    auto menu_owner = std::make_unique<app::Menu>(window, *renderer, input, settings_path);
    app::Menu& menu = *menu_owner;
    if (!menu.ok()) fail("the settings menu couldn't start on this window's Vulkan device");

    game::Scheduler scheduler(machine);
    const double frame_ns = 166400.0 / 10e6 * 1e9;
    std::uint64_t start = SDL_GetTicksNS(), paced_from = 0;
    bool quit = false;
    // The window's events, a frame at a time: the controls', the menu's, apb's own.
    auto events = [&] {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            input.handle(e);
            menu.handle(e);
            switch (e.type) {
            case SDL_EVENT_QUIT: quit = true; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_F2 && !e.key.repeat) input.test_switch = !input.test_switch;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                renderer->resize_window(static_cast<std::uint32_t>(e.window.data1), static_cast<std::uint32_t>(e.window.data2));
                break;
            default: break;
            }
        }
        if (menu.quit_requested()) quit = true;
    };
    // The controls change at the start of each vblank, on the cycle MAME's
    // inputs would (and qilin_run's scripted ones do).
    machine.on_vblank = [&](std::uint64_t) { input.apply(machine.inputs); };
    scheduler.on_frame = [&](std::uint64_t frame) {
        events();
        if (quit) throw game::Stop{game::Stop::frames, "closed"};
        if (sound) {
            machine.advance_speech(machine.sound_time);
            audio.advance(machine.sound_time);
            auto& made = audio.frames();
            // Keep the queue between one and four frames: drop what would
            // overfill it (the device's clock runs a little slow), and let
            // it fill again when it runs low.
            const int queued = SDL_GetAudioStreamQueued(sound);
            if (queued < 4 * frame_bytes) SDL_PutAudioStreamData(sound, made.data(), static_cast<int>(made.size() * sizeof(std::int16_t)));
            made.clear();
        }
        sys2::to_bgra(video.frame(), machine.palette(), pixels.data());
        // The menu pauses the game: the same frame, the menu over it, until it closes.
        while (menu.is_open() && !quit) {
            renderer->write_back_buffer(pixels.data());
            menu.frame();
            renderer->present();
            if (!renderer->config().vsync) SDL_Delay(16);
            events();
            input.apply(machine.inputs);   // keeps the meters live; the cabinet's controls rest
            paced_from = frame;
            start = SDL_GetTicksNS();
        }
        if (quit) throw game::Stop{game::Stop::frames, "closed"};
        renderer->write_back_buffer(pixels.data());
        menu.frame();
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
    if (sound) SDL_DestroyAudioStream(sound);
    machine.on_vblank = nullptr;
    menu_owner.reset();
    if (!input.settings.save(settings_path)) std::fprintf(stderr, "apb: couldn't write the settings to %s\n", settings_path.c_str());
    input_owner.reset();
    renderer.reset();
    SDL_DestroyWindow(window);
    if (!quit && stop.reason != game::Stop::frames) {
        const char* reasons[] = {"the frame count", "the watchdog", "unlifted main-CPU code", "unlifted sound-CPU code"};
        fail(app::strf("the game stopped at frame %llu: %s%s%s", static_cast<unsigned long long>(machine.frames()), reasons[stop.reason], stop.detail.empty() ? "" : ": ", stop.detail.c_str()));
    }
    SDL_Quit();
    return 0;
}
