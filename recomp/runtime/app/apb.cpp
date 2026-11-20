// apb: the game, recompiled. A window (SDL3), the board's frames drawn by the
// reference renderer and shown through Vulkan, and the cabinet's controls.
//
//   apb [rom-dir] [--scale N] [--no-vsync] [--gpu N] [--frames N] [--mute]
//
// With no rom-dir, the folder used last time (kept in apb.ini); if there's
// none, or it no longer holds a verified set, apb asks with a folder picker.
//
// Controls (keyboard; gamepads, wheels, pedals and mice too):
//   Left / Right       steering wheel          (left stick; a wheel; a mouse or spinner)
//   Up                 gas pedal               (right trigger; a pedal)
//   Left Ctrl or Z     siren / start           (A / south)
//   Left Alt or X      fire                    (B / east)
//   Space or C         siren (alternate)       (X / west)
//   5, 6               coin slots 1 and 2      (Back)
//   9                  service coin
//   F2                 the self-test switch (on/off)
//   F1 or Escape       the settings menu       (Start or Guide)
//   Alt+Enter or F11   full screen (on/off)
// The menu sets up wheels (any: a direct-drive wheel that spins freely, as
// the cabinet's did, or one that stops at a lock), pedals and extra button
// bindings, and the display: full screen, which display, the picture's size
// (whole multiples, fit or fill; sharp or smooth), its look (plain, smoothed
// pixel art, or a CRT), presets, vsync, the speed (the cabinet's exact 60.1
// Hz, the newest frame shown at each refresh; or locked to a display near
// 60 Hz or a multiple, every frame shown in turn) and the graphics device.
// They're kept in apb.ini next to the EEPROM; --no-vsync and --gpu N
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <filesystem>
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
    int i = 1;
    if (argc > 1 && std::string_view(argv[1]).substr(0, 2) != "--") o.rom_dir = argv[i++];   // else the saved folder, or ask
    for (; i < argc; ++i) {
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

// A ROM set's error for a message box: the first lines, and how many more.
std::string short_error(const std::string& error) {
    std::string out;
    std::size_t lines = 0, at = 0;
    while (at < error.size()) {
        const std::size_t end = std::min(error.find('\n', at), error.size());
        if (++lines <= 4) out += (lines > 1 ? "\n" : "") + error.substr(at, end - at);
        at = end + 1;
    }
    if (lines > 4) out += app::strf("\n  (and %zu more)", lines - 4);
    return out;
}

// Asks for the ROM folder with the system's folder picker, saying why.
// Nothing if the player cancels (or no picker can be shown: the error says so).
std::optional<std::string> pick_rom_folder(const std::string& why, const std::string& start) {
    SDL_MessageBoxButtonData buttons[2]{};
    buttons[0] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose the folder"};
    buttons[1] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"};
    const std::string text = why + "\n\nAPB needs the rev 7 ROM set (all 36 files, as MAME's apb set). Choose the folder that holds them.";
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.title = "APB: the ROMs";
    box.message = text.c_str();
    box.numbuttons = 2;
    box.buttons = buttons;
    int chosen = 0;
    if (!SDL_ShowMessageBox(&box, &chosen) || chosen != 1) return std::nullopt;
    struct Pick {
        bool done = false;
        std::optional<std::string> folder;
        std::string error;
    } pick;
    SDL_ShowOpenFolderDialog(
        [](void* user, const char* const* list, int) {
            auto* p = static_cast<Pick*>(user);
            if (!list) p->error = SDL_GetError();
            else if (list[0]) p->folder = list[0];
            p->done = true;
        },
        &pick, nullptr, start.empty() ? nullptr : start.c_str(), false);
    while (!pick.done) {   // the dialog may answer on another thread, or need events pumped
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    if (!pick.error.empty()) {
        std::fprintf(stderr, "apb: no folder picker: %s\n", pick.error.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "APB", ("No folder picker could be shown (" + pick.error +
                                 "). Give the ROM folder on the command line: apb <rom-dir>").c_str(), nullptr);
    }
    return pick.folder;
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
        std::fprintf(stderr, "usage: apb [rom-dir] [--scale N] [--no-vsync] [--gpu N] [--frames N] [--mute]\n"
                             "  (no rom-dir: the folder used last time, else apb asks for it)\n");
        return 2;
    }
    // Controllers count when the window isn't the active one too: started
    // from a terminal, the focus can stay there, and SDL would otherwise
    // ignore every controller until the window is clicked.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "apb: SDL: %s\n", SDL_GetError());
        return 1;
    }
    // The player's settings: the controls (for the input below), the display and the ROM folder.
    const std::string settings_path = pref_path("apb.ini");
    app::Settings saved;
    saved.load(settings_path);

    // The ROMs: the folder given, else the one used last time, else ask (and
    // ask again while the folder chosen doesn't hold a verified set).
    std::string rom_dir = options.rom_dir ? options.rom_dir : saved.rom_dir;
    auto set = rom::RomSet::load(rom_dir.empty() ? std::filesystem::path(".") : std::filesystem::path(rom_dir));
    if (!set && options.rom_dir) fail(set.error());   // given on the command line: say what's wrong with it
    std::string why;
    if (!set) why = rom_dir.empty() ? "Where are your APB ROMs?" : "The ROMs in " + rom_dir + " aren't a complete rev 7 set: " + short_error(set.error());
    while (!set) {
        const auto folder = pick_rom_folder(why, rom_dir);
        if (!folder) {
            SDL_Quit();
            return 0;
        }
        rom_dir = *folder;
        set = rom::RomSet::load(rom_dir);
        if (!set) why = "The ROMs in " + rom_dir + " aren't a complete rev 7 set: " + short_error(set.error());
    }
    if (rom_dir != saved.rom_dir) {
        saved.rom_dir = rom_dir;
        if (!saved.save(settings_path)) std::fprintf(stderr, "apb: couldn't write the settings to %s\n", settings_path.c_str());
    }

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
    config.lock_to_display = app::display_repeats(window, saved.display) > 0;
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
    for (const auto& d : input.devices()) menu.notify("Controller: " + d.name);
    if (input.devices().empty()) menu.notify("No controller found yet (the keyboard works)");

    game::Scheduler scheduler(machine);
    const double frame_ns = 166400.0 / 10e6 * 1e9;
    std::uint64_t start = SDL_GetTicksNS(), paced_from = 0;
    // Locked to the display ("match the display"): how many times each frame
    // is shown, at what refresh, and a check that the display really paces it.
    int lock_repeats = 0, lock_frames = 0;
    double lock_refresh = 0;
    std::uint64_t lock_since = 0;
    bool lock_failed = false;
    bool quit = false;
    // The window's events, a frame at a time: the controls', the menu's, apb's own.
    auto events = [&] {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // Controllers coming and going, shown in the window for a few seconds.
            std::string leaving;
            if (e.type == SDL_EVENT_JOYSTICK_REMOVED)
                for (const auto& d : input.devices())
                    if (d.id == e.jdevice.which) leaving = d.name;
            input.handle(e);
            menu.handle(e);
            if (!leaving.empty()) menu.notify("Controller disconnected: " + leaving);
            if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
                bool opened = false;
                for (const auto& d : input.devices())
                    if (d.id == e.jdevice.which) {
                        menu.notify("Controller connected: " + d.name);
                        opened = true;
                    }
                if (!opened) {
                    const char* name = SDL_GetJoystickNameForID(e.jdevice.which);
                    menu.notify(std::string("Controller found but couldn't be opened: ") + (name ? name : "(no name)"));
                }
            }
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
        // Locked to the display ("match the display"), presenting paces the
        // game; otherwise the board's clock does.
        double refresh = 0;
        const int possible = app::display_repeats(window, input.settings.display, &refresh);
        if (lock_failed && (!input.settings.display.match_display || refresh != lock_refresh)) {   // turned off, or another display: try again
            lock_failed = false;
            menu.speed_note.clear();
        }
        const int repeats = lock_failed ? 0 : possible;
        if (repeats != lock_repeats || refresh != lock_refresh) {
            lock_repeats = repeats;
            lock_refresh = refresh;
            lock_frames = -30;   // measuring starts half a second in, once the swapchain is rebuilt
        }
        renderer->set_lock_to_display(repeats > 0);
        // The menu pauses the game: the same frame, the menu over it, until it closes.
        while (menu.is_open() && !quit) {
            renderer->write_back_buffer(pixels.data());
            menu.frame();
            renderer->present();
            if (!renderer->config().lock_to_display || !renderer->config().vsync) SDL_Delay(16);   // only a locked FIFO waits by itself
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
        if (repeats > 0 && renderer->config().lock_to_display) {
            renderer->present();
            for (int i = 1; i < repeats; ++i) renderer->present_again();
            const auto now = SDL_GetTicksNS();
            start = now;   // so the clock takes over smoothly if the lock ends
            paced_from = frame + 1;
            // The display must be what paces the game: if presenting doesn't wait
            // for it (a remote or virtual display, a driver ignoring vsync), the
            // game would race. Two seconds more than 2% off its rate ends the lock.
            if (++lock_frames == 0) lock_since = now;
            if (lock_frames == 120) {
                const double measured = 120.0 * 1e9 / static_cast<double>(now - lock_since);
                const double expected = lock_refresh / repeats;
                if (std::abs(measured / expected - 1) > 0.02) {
                    lock_failed = true;
                    menu.speed_note = app::strf("Locking didn't hold: this display took %.1f frames a second, not %.2f. The exact speed is used.",
                                                measured, expected);
                    std::fprintf(stderr, "apb: %s\n", menu.speed_note.c_str());
                }
                lock_frames = 0;
                lock_since = now;
            }
            return;
        }
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
