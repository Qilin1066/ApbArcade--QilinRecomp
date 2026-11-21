#include "menu.hpp"
#include "persistence.hpp"
#include <filesystem>
#include "text.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#ifndef QILIN_APP_VERSION
#define QILIN_APP_VERSION "?"
#endif

namespace qilin::app {

namespace {

constexpr const char* hint_text = "F1 or Esc (controller: Back + Start): settings";

// The window's format, which ImGui's pipeline info points at for as long as it lives.
VkFormat window_format = VK_FORMAT_UNDEFINED;

void check_vk(VkResult r) {
    if (r != VK_SUCCESS) std::fprintf(stderr, "apb: Vulkan error %d in the menu\n", static_cast<int>(r));
}

// A bar from -1 to 1 (or 0 to 1), with a label.
void meter(const char* label, double value, bool centred) {
    const float fraction = centred ? static_cast<float>((std::clamp(value, -1.0, 1.0) + 1) / 2) : static_cast<float>(std::clamp(value, 0.0, 1.0));
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), strf("%s %+.3f", label, value).c_str());
}

}  // namespace

SDL_DisplayID display_named(const std::string& name) {
    if (name.empty()) return 0;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    SDL_DisplayID found = 0;
    for (int i = 0; i < count && displays && !found; ++i) {
        const char* n = SDL_GetDisplayName(displays[i]);
        if (n && name == n) found = displays[i];
    }
    SDL_free(displays);
    return found;
}

int repeats_for(double refresh_hz) {
    if (!(refresh_hz > 0)) return 0;
    const auto k = static_cast<int>(std::lround(refresh_hz / game_hz));
    if (k < 1 || k > 4) return 0;
    return std::abs(refresh_hz / k - game_hz) / game_hz <= 0.01 ? k : 0;
}

int display_repeats(SDL_Window* window, const DisplaySettings& d, double* refresh) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    const double hz = mode ? static_cast<double>(mode->refresh_rate) : 0.0;
    if (refresh) *refresh = hz;
    return d.vsync && d.match_display ? repeats_for(hz) : 0;
}

Menu::Menu(SDL_Window* window, gfx::Renderer& renderer, Input& input, std::string settings_path)
    : window_(window), renderer_(renderer), input_(input), path_(std::move(settings_path)) {
    gfx::Overlay o;
    if (!renderer_.overlay(o)) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // the menu keeps no layout file; the settings file is apb's
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    // 1.25x, as the other recomps have it, times the display's own scale.
    float dpi = SDL_GetWindowDisplayScale(window_);
    if (!(dpi > 0.0f)) dpi = 1.0f;
    ImGui::GetStyle().ScaleAllSizes(1.25f * dpi);
    ImGui::GetStyle().FontScaleMain = 1.25f;
    ImGui::GetStyle().FontScaleDpi = dpi;
    if (!ImGui_ImplSDL3_InitForVulkan(window_)) return;
    sdl_backend_ = true;

    window_format = static_cast<VkFormat>(o.format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = static_cast<VkInstance>(o.instance);
    info.PhysicalDevice = static_cast<VkPhysicalDevice>(o.physical_device);
    info.Device = static_cast<VkDevice>(o.device);
    info.QueueFamily = o.queue_family;
    info.Queue = static_cast<VkQueue>(o.queue);
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
    info.MinImageCount = std::max<std::uint32_t>(2, o.min_images);
    info.ImageCount = std::max(info.MinImageCount, o.images);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfo{};
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &window_format;
    info.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&info)) return;
    ok_ = true;
    hint_until_ = SDL_GetTicks() + 8000;
    std::fprintf(stderr, "apb: %s\n", hint_text);
    // Steering by mouse or spinner: it's captured while playing.
    SDL_SetWindowRelativeMouseMode(window_, input_.settings.steering == Steering::mouse);
}

Menu::~Menu() {
    renderer_.set_overlay({});
    if (ok_) ImGui_ImplVulkan_Shutdown();
    if (sdl_backend_) ImGui_ImplSDL3_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

void Menu::set_open(bool open) {
    if (open == open_) return;
    open_ = open;
    input_.set_playing(!open);
    input_.end_wheel_measurement();
    input_.stop_listening();
    listening_button_ = -1;
    finding_wheel_ = false;
    measuring_ = false;
    peek_ = close_asked_ = false;
    SDL_SetWindowRelativeMouseMode(window_, !open && input_.settings.steering == Steering::mouse);
    if (!open) save();   // closing the menu keeps what was set
}

void Menu::save() {
    if (!input_.settings.save(path_)) std::fprintf(stderr, "apb: couldn't write the settings to %s\n", path_.c_str());
}

void Menu::handle(const SDL_Event& e) {
    if (ok_ && (open_ || SDL_GetTicks() < hint_until_ || !notices_.empty())) ImGui_ImplSDL3_ProcessEvent(&e);
    switch (e.type) {
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: input_.settings.display.fullscreen = true; break;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: input_.settings.display.fullscreen = false; break;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat) break;
        if (peek_) {   // looking at the picture: any key brings the menu back
            peek_ = false;
            break;
        }
        if (e.key.key == SDLK_F3) {   // the frame-time readout, kept in the settings
            input_.settings.display.performance_overlay = !input_.settings.display.performance_overlay;
            save();
        }
        if (e.key.key == SDLK_F1) toggle();
        if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
            set_fullscreen(!input_.settings.display.fullscreen);
            save();
        }
        // Escape opens the menu, or closes it (unless a control is being set:
        // then it gives up waiting for one).
        if (e.key.key == SDLK_ESCAPE) {
            if (open_ && (input_.listening() != Input::Listen::none || measuring_)) {
                input_.stop_listening();
                listening_button_ = -1;
                finding_wheel_ = false;
                measuring_ = false;
                input_.end_wheel_measurement();
            } else {
                toggle();
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        peek_ = false;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        if (peek_) {
            peek_ = false;
            break;
        }
        // Back and Start together, as in the other recomps (Back alone is
        // coin 1; Input holds it back while Start is pressed with it); with
        // the menu open, Start alone closes it too.
        SDL_Gamepad* pad = SDL_GetGamepadFromID(e.gbutton.which);
        const bool back = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
        const bool start = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
        if ((back && start) || (open_ && e.gbutton.button == SDL_GAMEPAD_BUTTON_START)) toggle();
        break;
    }
    default: break;
    }
}

void Menu::notify(std::string text) {
    notices_.push_back({std::move(text), SDL_GetTicks() + 5000});
    if (notices_.size() > 4) notices_.erase(notices_.begin());
}

void Menu::frame() {
    const std::uint64_t now = SDL_GetTicks();
    // Every shown frame's interval, for F3 (recorded whether it's showing or not).
    const std::uint64_t ns = SDL_GetTicksNS();
    if (last_frame_ns_ && !open_) {
        frame_ms_[frame_next_] = static_cast<float>(static_cast<double>(ns - last_frame_ns_) / 1e6);
        frame_next_ = (frame_next_ + 1) % frame_history;
        frame_count_ = std::min(frame_count_ + 1, frame_history);
    }
    last_frame_ns_ = open_ ? 0 : ns;   // the paused menu's frames aren't the game's
    std::erase_if(notices_, [&](const Notice& n) { return n.until <= now; });
    const bool hint = !open_ && (now < hint_until_ || !notices_.empty());
    const bool times = !open_ && input_.settings.display.performance_overlay;
    if (!ok_ || (!open_ && !hint && !times) || (open_ && peek_)) {
        if (drawing_) {
            renderer_.set_overlay({});
            drawing_ = false;
        }
        return;
    }
    if (!drawing_) {
        renderer_.set_overlay([](void* cmd) {
            if (ImDrawData* data = ImGui::GetDrawData()) ImGui_ImplVulkan_RenderDrawData(data, static_cast<VkCommandBuffer>(cmd));
        });
        drawing_ = true;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiViewport* view = ImGui::GetMainViewport();
    if (!open_) {
        if (hint) {
            ImGui::SetNextWindowPos(ImVec2(view->WorkPos.x + 12, view->WorkPos.y + view->WorkSize.y - 12), ImGuiCond_Always, ImVec2(0, 1));
            ImGui::SetNextWindowBgAlpha(0.6f);
            ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
            for (const auto& n : notices_) ImGui::TextUnformatted(n.text.c_str());
            if (now < hint_until_) ImGui::TextUnformatted(hint_text);
            ImGui::End();
        }
        if (times) draw_frame_times();
    } else {
        ImGui::SetNextWindowPos(view->WorkPos);
        ImGui::SetNextWindowSize(view->WorkSize);
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        constexpr const char* title = "APB - settings (the game is paused)";
        ImGui::TextUnformatted(title);
        // Close and Quit, top right on every page, as in the other recomps
        // (on a line of their own when the window is too narrow for both:
        // APB's own window is upright, 384 pixels wide at 1x).
        const ImGuiStyle& st = ImGui::GetStyle();
        const float buttons = ImGui::CalcTextSize("Close (Esc)").x + ImGui::CalcTextSize("Quit the game").x + 4 * st.FramePadding.x + st.ItemSpacing.x;
        const float width = ImGui::GetContentRegionAvail().x;
        if (ImGui::CalcTextSize(title).x + st.ItemSpacing.x + buttons <= width) ImGui::SameLine(width - buttons + st.WindowPadding.x);
        else ImGui::SetCursorPosX(std::max(st.WindowPadding.x, width - buttons + st.WindowPadding.x));
        if (ImGui::Button("Close (Esc)")) close_asked_ = true;   // closed once the frame is built
        ImGui::SameLine();
        if (ImGui::Button("Quit the game")) quit_ = true;
        ImGui::Separator();
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);   // sliders take half; their labels the rest
        ImGui::PushTextWrapPos(0.0f);   // APB's window is upright and narrow: text wraps
        if (ImGui::BeginTabBar("pages")) {
            if (ImGui::BeginTabItem("Controls")) {
                controls_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Display")) {
                display_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Game")) {
                game_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopTextWrapPos();
        ImGui::PopItemWidth();
        ImGui::End();
    }
    ImGui::Render();
    if (close_asked_) {
        close_asked_ = false;
        set_open(false);   // saves; the frame already built is still drawn
    }
}

// F3: how evenly frames reach the screen, top left, as in the other recomps.
// The lows are the frame rates of the slowest 1% and 0.1% of frames over the
// last 20 s.
void Menu::draw_frame_times() {
    const ImGuiViewport* view = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(view->WorkPos.x + 8.0f, view->WorkPos.y + 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##frame times", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                               ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (frame_count_ < 2) {
        ImGui::TextUnformatted("frame times: waiting for frames");
    } else {
        std::vector<float> sorted;
        sorted.reserve(frame_count_);
        double total = 0;
        for (std::size_t i = 0; i < frame_count_; ++i) {
            const float ms = frame_ms_[(frame_next_ + frame_history - 1 - i) % frame_history];
            sorted.push_back(ms);
            total += ms;
        }
        std::ranges::sort(sorted, std::greater<>());
        const double average = total / static_cast<double>(frame_count_);
        const auto low = [&](double share) {   // the frame rate of the slowest share of frames
            const auto n = std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<double>(sorted.size()) * share));
            double sum = 0;
            for (std::size_t i = 0; i < n; ++i) sum += sorted[i];
            return 1000.0 / (sum / static_cast<double>(n));
        };
        ImGui::Text("%.1f fps  %.2f ms", 1000.0 / average, average);
        ImGui::Text("1%% low %.1f  0.1%% low %.1f", low(0.01), low(0.001));
        ImGui::Text("worst %.1f ms", static_cast<double>(sorted.front()));
        // The game makes 60.1 frames a second, as the cabinet did; "match the
        // display" shows each one a whole number of times.
        double hz = 0;
        const int repeats = display_repeats(window_, input_.settings.display, &hz);
        if (repeats > 1) ImGui::Text("display %.0f Hz: each frame shown %d times", hz, repeats);
        else if (hz > 0) ImGui::Text("display %.0f Hz%s", hz, repeats == 1 ? ": locked" : "");
        std::vector<float> recent(std::min<std::size_t>(frame_count_, 240));
        for (std::size_t i = 0; i < recent.size(); ++i)
            recent[recent.size() - 1 - i] = frame_ms_[(frame_next_ + frame_history - 1 - i) % frame_history];
        ImGui::PlotLines("##ft", recent.data(), static_cast<int>(recent.size()), 0, nullptr, 0.0f, 50.0f, ImVec2(240.0f, 48.0f));
    }
    ImGui::End();
}

void Menu::controls_tab() {
    ImGui::SeparatorText("Controllers");
    if (input_.devices().empty()) ImGui::TextDisabled("None connected: the keyboard plays the cabinet.");
    for (const auto& d : input_.devices()) {
        ImGui::BulletText("%s  (%s; %d axes, %d buttons%s)", d.name.c_str(), Input::type_name(d.type), SDL_GetNumJoystickAxes(d.joystick),
                          SDL_GetNumJoystickButtons(d.joystick), SDL_IsJoystickHaptic(d.joystick) ? "; force feedback" : "");
    }
    if (ImGui::Button("Save controller diagnostics")) {
        auto path = std::filesystem::path(std::u8string(path_.begin(), path_.end())).parent_path() / "controllers.txt";
        const auto utf8 = path.u8string();
        const std::string output(utf8.begin(), utf8.end());
        std::string error;
        if (persist::replace(output, input_.diagnostics(), &error)) notify("Controller report saved: " + output);
        else notify(error);
    }
    steering_section();
    pedal_section();
    buttons_section();
    ImGui::Separator();
    if (ImGui::Button("Back to the defaults")) {
        const DisplaySettings display = input_.settings.display;   // the display and the ROM folder aren't controls
        const std::string rom_dir = input_.settings.rom_dir;
        input_.settings = Settings{};
        input_.settings.display = display;
        input_.settings.rom_dir = rom_dir;
        input_.stop_listening();
        input_.end_wheel_measurement();
        measuring_ = finding_wheel_ = false;
        listening_button_ = -1;
        input_.configure();
    }
    ImGui::TextDisabled("Escape, F1, F2, F3 and F11 are kept for the program.");
    ImGui::TextDisabled("F1 or Esc (controller: Back + Start): this menu. F2: the self-test switch. F3: frame times. F11 or Alt+Enter: full screen.");
}

void Menu::steering_section() {
    ImGui::SeparatorText("Steering");
    auto& s = input_.settings;
    ImGui::TextWrapped("APB's wheel spins freely: the game counts how far it turns, not where it is. The arrow keys and a "
                       "gamepad's left stick always steer too.");
    int mode = static_cast<int>(s.steering);
    ImGui::RadioButton("Keys and gamepad only", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Wheel", &mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Mouse or spinner", &mode, 2);
    if (mode != static_cast<int>(s.steering)) {
        measuring_ = false;
        input_.end_wheel_measurement();
        s.steering = static_cast<Steering>(mode);
        input_.configure();
    }

    if (s.steering == Steering::wheel) {
        if (!s.profiles.empty() && ImGui::BeginCombo("Saved wheel setup", s.wheel_axis.name.empty() ? "Choose wheel" : s.wheel_axis.name.c_str())) {
            std::string chosen;
            for (const auto& [guid, profile] : s.profiles) {
                const std::string label = (profile.wheel_axis.name.empty() ? "Wheel " + guid.substr(0, 8) : profile.wheel_axis.name) + "##" + guid;
                if (ImGui::Selectable(label.c_str(), guid == s.wheel_axis.device)) chosen = guid;
            }
            ImGui::EndCombo();
            if (!chosen.empty()) {
                s.remember_controls();
                s.select_profile(chosen);
                input_.configure();
            }
        }
        ImGui::TextWrapped("Each bound wheel remembers its calibration, pedals, buttons and friction. Display settings stay shared.");
        int behaviour = (s.moza_angle || s.wheel.wrap) ? 0 : 1;
        if (ImGui::Combo("Wheel behaviour", &behaviour, "Free rotation / direct drive\0Limited rotation / belt or gear driven\0")) {
            measuring_ = false;
            input_.end_wheel_measurement();
            s.moza_angle = false;
            s.wheel.wrap = behaviour == 0;
            input_.configure();
        }
        ImGui::TextWrapped("Choose how your wheel is configured. A direct-drive wheel with a software stop can use Limited rotation too.");
        if (ImGui::SmallButton("MOZA R5 (free rotation)")) {
            measuring_ = false;
            input_.end_wheel_measurement();
            s.moza_angle = true;
            s.wheel.wrap = false;
            input_.configure();
        }
        if (ImGui::SmallButton("Logitech G920 (900 degrees)")) {
            measuring_ = false;
            input_.end_wheel_measurement();
            s.moza_angle = false;
            s.wheel.wrap = false;
            s.wheel.rotation = 900;
            s.wheel.edge = 0.92;
            input_.configure();
        }
        const bool limited = !s.moza_angle && !s.wheel.wrap;
        if (s.moza_angle && ImGui::BeginCombo("Wheel device", s.wheel_axis.name.empty() ? "Choose the MOZA base" : s.wheel_axis.name.c_str())) {
            for (const auto& d : input_.devices()) {
                if (SDL_GetNumJoystickAxes(d.joystick) == 0) continue;
                ImGui::PushID(static_cast<int>(d.id));
                if (ImGui::Selectable(d.name.c_str(), s.wheel_axis.device == d.guid)) {
                    s.wheel_axis = Binding{Binding::Kind::axis, d.guid, d.name, 0, 0};
                    input_.configure();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::Text("Wheel: %s", s.wheel_axis.describe().c_str());
        ImGui::SameLine();
        if (input_.listening() == Input::Listen::wheel) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Turn the wheel...");
        } else if (ImGui::Button("Find it")) {
            measuring_ = false;
            input_.end_wheel_measurement();
            listening_button_ = -1;
            finding_wheel_ = true;
            input_.listen(Input::Listen::wheel);
        }
        if (!s.wheel_axis.empty()) {
            meter("position", input_.read(s.wheel_axis), true);
            ImGui::Text("Dial counts this frame: %+d", input_.last_counts);
        }
        bool changed = false;
        float rotation = static_cast<float>(s.wheel.rotation);
        if (limited) {
            if (ImGui::InputFloat("Maximum angle (degrees)", &rotation, 90, 360, "%.0f")) {
                if (std::isfinite(rotation) && rotation >= 90 && rotation <= 10000) changed = true;
                else rotation = static_cast<float>(s.wheel.rotation);
            }
            ImGui::TextWrapped("Total travel from left stop to right stop: 900 degrees means 450 each way. Match your wheel driver's setting (90-10000 degrees).");
            for (const int preset : {360, 540, 900, 1080, 1440, 2160, 2700}) {
                if (preset != 360) ImGui::SameLine();
                if (ImGui::SmallButton(std::to_string(preset).c_str())) {
                    rotation = static_cast<float>(preset);
                    changed = true;
                }
            }
        } else {
            ImGui::TextWrapped("Free rotation has no maximum steering angle. The input must keep reporting movement through repeated turns.");
        }
        if (s.moza_angle) {
            ImGui::TextWrapped("%s", input_.moza_status().c_str());
            if (const auto angle = input_.moza_degrees()) ImGui::Text("Physical angle: %.2f degrees", *angle);
        }
        float counts = static_cast<float>(s.wheel.counts_per_turn);
        changed |= ImGui::SliderFloat("Sensitivity (dial counts per turn)", &counts, 32, 512, "%.0f");
        bool edge = s.wheel.edge < 1.0;
        if (limited)
            changed |= ImGui::Checkbox("Keep turning while held at a stop", &edge);
        float edge_start = static_cast<float>(s.wheel.edge < 1.0 ? s.wheel.edge : controls::WheelSettings{}.edge);
        float edge_rate = static_cast<float>(s.wheel.edge_rate);
        if (limited && edge) {
            changed |= ImGui::SliderFloat("Starts this far to the stop", &edge_start, 0.6f, 0.99f, "%.2f");
            changed |= ImGui::SliderFloat("Turning speed at the stop (counts a frame)", &edge_rate, 1, 16, "%.1f");
        }
        changed |= ImGui::Checkbox("Reverse the wheel", &s.wheel.invert);
        if (ImGui::CollapsingHeader("Advanced wheel input", measuring_ ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            if (!limited) {
                int source = s.moza_angle ? 0 : 1;
                if (ImGui::Combo("Continuous input source", &source, "MOZA physical angle (Windows SDK)\0Wrapping joystick axis\0")) {
                    s.moza_angle = source == 0;
                    s.wheel.wrap = source == 1;
                    measuring_ = false;
                    input_.end_wheel_measurement();
                    input_.configure();
                }
            }
            if (s.moza_angle) {
                ImGui::TextWrapped("Use MOZA SDK 1.0.1.8's MSVC2022-64 MOZA_SDK.dll beside apb.exe, with SDK-enabled Pit House running. "
                                   "This reads physical degrees independently of the joystick's limited axis.");
                if (ImGui::Button("Retry MOZA connection")) input_.retry_moza();
            } else {
                if (s.wheel.wrap) {
                    changed |= ImGui::SliderFloat("Degrees per reported axis cycle", &rotation, 90, 10000, "%.0f");
                    ImGui::TextWrapped("This calibrates the input scale; it does not limit physical rotation. Use a wrapping axis only if its reading rolls "
                                       "from one endpoint to the other as you keep turning. A clipped axis needs a physical-angle source.");
                }
                if (!s.wheel_axis.empty()) {
                    if (!measuring_) {
                        if (ImGui::Button("Measure: turn the wheel one full turn")) {
                            measuring_ = true;
                            input_.begin_wheel_measurement();
                        }
                    } else {
                        const double moved = input_.measured_wheel_travel();
                        ImGui::TextWrapped("Turn the wheel exactly one full turn (360 degrees), then press Done. Moved so far: %.3f", moved);
                        if (ImGui::Button("Done") && std::abs(moved) > 0.05) {
                            rotation = static_cast<float>(std::clamp(720.0 / std::abs(moved), 90.0, 10000.0));
                            changed = true;
                            measuring_ = false;
                            input_.end_wheel_measurement();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) { measuring_ = false; input_.end_wheel_measurement(); }
                    }
                }
            }
        }
        if (changed) {
            s.wheel.rotation = rotation;
            s.wheel.counts_per_turn = counts;
            s.wheel.edge = edge ? edge_start : 1.0;
            s.wheel.edge_rate = edge_rate;
            input_.configure();
        }
        if (ImGui::SliderInt("Force feedback: friction (%)", &s.friction, 0, 100)) input_.configure();
        for (const auto& d : input_.devices())
            if (d.guid == s.wheel_axis.device && !d.force_status.empty()) ImGui::TextWrapped("%s", d.force_status.c_str());
        ImGui::TextWrapped("The game requests no centring spring and passive friction. Settings in the wheel's own driver can still add a spring or a stop.");
    } else if (s.steering == Steering::mouse) {
        float m = static_cast<float>(s.mouse_counts);
        if (ImGui::SliderFloat("Dial counts per mouse step", &m, 0.02f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) s.mouse_counts = m;
        ImGui::TextDisabled("While playing, the mouse is captured; the menu frees it.");
    }
    ImGui::SeparatorText("Gamepad stick and arrow keys");
    float stick = static_cast<float>(s.stick_speed);
    if (ImGui::SliderFloat("Turning speed (dial counts a frame, stick fully over)", &stick, 0.5f, 8, "%.1f")) {
        s.stick_speed = stick;
        input_.configure();
    }
    float curve = static_cast<float>(s.stick_curve);
    if (ImGui::SliderFloat("Stick response (1 straight; higher: finer near the centre)", &curve, 1, 4, "%.1f")) {
        s.stick_curve = curve;
        input_.configure();
    }
    ImGui::TextDisabled("%s", strf("Half way over turns at %.0f%% of full speed. The cabinet's wheel is 160 counts a turn: full speed is %.1f turns a second.",
                                   100 * std::pow((0.5 - 0.15) / 0.85, s.stick_curve), s.stick_speed * 60.1 / 160).c_str());

    // What listening for the wheel found (only when it was the wheel being looked for).
    if (finding_wheel_ && input_.listening() == Input::Listen::none) {
        finding_wheel_ = false;
        if (auto heard = input_.heard(); heard && heard->kind == Binding::Kind::axis) {
            s.wheel_axis = *heard;
            s.wheel_axis.value = 0;
            input_.configure();
        }
    }
}

void Menu::pedal_section() {
    ImGui::SeparatorText("Gas pedal");
    auto& s = input_.settings;
    ImGui::TextWrapped("The Up key and a gamepad's right trigger always work. A pedal (or any axis) can be added:");
    ImGui::Text("Pedal: %s", s.pedal_axis.describe().c_str());
    if (input_.listening() == Input::Listen::pedal) {
        const Binding& found = input_.pedal_candidate();
        const auto [rest, far] = input_.pedal_range();
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Press the pedal all the way down and let it go, then press Done.");
        if (!found.empty()) ImGui::Text("Found %s: released %.3f, floored %.3f", found.describe().c_str(), rest, far);
        if (ImGui::Button("Done") && !found.empty()) {
            s.pedal_axis = found;
            s.pedal.released = rest;
            s.pedal.floored = far;
            input_.stop_listening();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) input_.stop_listening();
    } else {
        if (ImGui::Button("Set up a pedal")) {
            listening_button_ = -1;
            finding_wheel_ = false;
            input_.listen(Input::Listen::pedal);
        }
        if (!s.pedal_axis.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Remove it")) s.pedal_axis = {};
        }
    }
    if (!s.pedal_axis.empty()) {
        float released = static_cast<float>(s.pedal.released), floored = static_cast<float>(s.pedal.floored);
        float dead = static_cast<float>(s.pedal.deadzone);
        if (ImGui::SliderFloat("Reading released", &released, -1, 1, "%.3f")) s.pedal.released = released;
        if (ImGui::SliderFloat("Reading floored", &floored, -1, 1, "%.3f")) s.pedal.floored = floored;
        if (ImGui::SliderFloat("Dead zone at each end", &dead, 0, 0.2f, "%.2f")) s.pedal.deadzone = dead;
        meter("axis", input_.read(s.pedal_axis), true);
    }
    meter("gas", input_.last_pedal, false);
}

void Menu::buttons_section() {
    ImGui::SeparatorText("Buttons and coins");
    auto& s = input_.settings;
    static constexpr const char* defaults[] = {"Left Ctrl, Z, gamepad A", "Left Alt, X, gamepad B", "Space, C, gamepad X",
                                               "5, gamepad Back", "6", "9"};
    if (ImGui::BeginTable("buttons", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        for (int i = 0; i < static_cast<int>(Button::count); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(button_names[i]);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", defaults[i]);
            ImGui::TableNextColumn();
            if (listening_button_ == i) ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Press it... (Esc: cancel)");
            else ImGui::TextUnformatted(s.buttons[i].empty() ? "" : s.buttons[i].describe().c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Set")) {
                listening_button_ = i;
                finding_wheel_ = false;
                input_.listen(Input::Listen::button);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) s.buttons[i] = {};
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (listening_button_ >= 0 && input_.listening() == Input::Listen::none) {
        if (auto heard = input_.heard()) s.buttons[listening_button_] = *heard;
        listening_button_ = -1;
    }
}

void Menu::set_fullscreen(bool fullscreen) {
    input_.settings.display.fullscreen = fullscreen;
    if (!SDL_SetWindowFullscreen(window_, fullscreen)) std::fprintf(stderr, "apb: full screen: %s\n", SDL_GetError());
}

void Menu::move_to_display(SDL_DisplayID display) {
    const bool fullscreen = input_.settings.display.fullscreen;
    if (fullscreen) SDL_SetWindowFullscreen(window_, false);
    SDL_SetWindowPosition(window_, static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(display)),
                          static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(display)));
    if (fullscreen) SDL_SetWindowFullscreen(window_, true);
    const char* name = SDL_GetDisplayName(display);
    input_.settings.display.monitor = name ? name : "";
}

void Menu::display_tab() {
    DisplaySettings& d = input_.settings.display;
    const auto apply = [&] {
        renderer_.set_scaling(d.scaling, d.smooth);
        renderer_.set_effect(d.effect, d.scanlines, d.mask);
        renderer_.set_vsync(d.vsync);
    };
    ImGui::SeparatorText("Window");
    bool fullscreen = d.fullscreen;
    if (ImGui::Checkbox("Full screen (F11 or Alt+Enter)", &fullscreen)) set_fullscreen(fullscreen);

    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    const SDL_DisplayID current = SDL_GetDisplayForWindow(window_);
    const char* current_name = current ? SDL_GetDisplayName(current) : nullptr;
    if (ImGui::BeginCombo("Display", current_name ? current_name : "?")) {
        for (int i = 0; i < count && displays; ++i) {
            const char* name = SDL_GetDisplayName(displays[i]);
            if (ImGui::Selectable(strf("%s##%u", name ? name : "?", static_cast<unsigned>(displays[i])).c_str(), displays[i] == current))
                move_to_display(displays[i]);
        }
        ImGui::EndCombo();
    }
    SDL_free(displays);

    ImGui::SeparatorText("Picture");
    bool changed = false;
    const int preset = matching_preset(d);
    if (ImGui::BeginCombo("Preset", preset < 0 ? "(set by hand)" : display_presets[static_cast<std::size_t>(preset)].name)) {
        for (std::size_t i = 0; i < display_presets.size(); ++i) {
            if (ImGui::Selectable(display_presets[i].name, preset == static_cast<int>(i))) {
                apply_preset(d, i);
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", display_presets[i].about);
        }
        ImGui::EndCombo();
    }
    int scaling = static_cast<int>(d.scaling);
    bool scaling_changed = ImGui::RadioButton("Whole multiples (sharpest)", &scaling, static_cast<int>(gfx::Scaling::integer));
    ImGui::SameLine();
    scaling_changed |= ImGui::RadioButton("Fit", &scaling, static_cast<int>(gfx::Scaling::fit));
    ImGui::SameLine();
    scaling_changed |= ImGui::RadioButton("Fill the window", &scaling, static_cast<int>(gfx::Scaling::stretch));
    if (scaling_changed) d.scaling = static_cast<gfx::Scaling>(scaling);
    changed |= scaling_changed;
    int effect = static_cast<int>(d.effect);
    bool effect_changed = ImGui::RadioButton("Plain", &effect, static_cast<int>(gfx::Effect::none));
    ImGui::SameLine();
    effect_changed |= ImGui::RadioButton("Smoothed pixel art", &effect, static_cast<int>(gfx::Effect::pixel_art));
    ImGui::SameLine();
    effect_changed |= ImGui::RadioButton("CRT television", &effect, static_cast<int>(gfx::Effect::crt));
    if (effect_changed) d.effect = static_cast<gfx::Effect>(effect);
    changed |= effect_changed;
    if (d.effect == gfx::Effect::none) {
        if (renderer_.smooth_supported()) {
            changed |= ImGui::Checkbox("Smooth", &d.smooth);
        } else {
            ImGui::TextDisabled("Smooth: this graphics device can't (the picture is scaled sharp).");
        }
    } else if (d.effect == gfx::Effect::pixel_art) {
        ImGui::TextDisabled("Edges between colours are drawn as lines instead of steps. Sharpest at Whole multiples.");
    } else {
        int scan = static_cast<int>(std::lround(d.scanlines * 100)), mask = static_cast<int>(std::lround(d.mask * 100));
        if (ImGui::SliderInt("Scanlines", &scan, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
            d.scanlines = static_cast<float>(scan) / 100.0f;
            changed = true;
        }
        if (ImGui::SliderInt("Phosphor stripes", &mask, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
            d.mask = static_cast<float>(mask) / 100.0f;
            changed = true;
        }
        ImGui::TextDisabled("APB's monitor stood upright, so its scanlines run down the screen. They show best at Whole "
                            "multiples, or a large window: they fade out below about two screen pixels per line.");
    }
    changed |= ImGui::Checkbox("Vsync", &d.vsync);
    ImGui::Checkbox("Frame times (F3)", &d.performance_overlay);
    if (changed) apply();

    ImGui::SeparatorText("Refresh rate");
    if (d.vsync) {
        int speed = d.match_display ? 1 : 0;
        bool speed_changed = ImGui::RadioButton("Exact (the cabinet's 60.1 Hz)", &speed, 0);
        ImGui::SameLine();
        speed_changed |= ImGui::RadioButton("Match the display (smoothest)", &speed, 1);
        if (speed_changed) d.match_display = speed == 1;
        double hz = 0;
        const int repeats = display_repeats(window_, d, &hz);
        if (d.match_display && !speed_note.empty()) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", speed_note.c_str());
        } else if (!d.match_display) {
            ImGui::TextDisabled("The newest frame is shown at each refresh. On a 60 Hz display one frame in about 600 is skipped, "
                                "as 60.1 doesn't divide into 60; a variable-refresh (FreeSync, G-Sync) display shows every one.");
        } else if (repeats) {
            const double game_rate = hz / repeats;
            ImGui::TextDisabled("%s", strf("Locked to this display's %.2f Hz%s: the game runs %.2f%% %s, every frame shown in turn.",
                                           hz, repeats > 1 ? strf(" (each frame shown %d times)", repeats).c_str() : "",
                                           std::abs(game_rate / game_hz - 1) * 100, game_rate < game_hz ? "slow" : "fast").c_str());
        } else {
            ImGui::TextDisabled("%s", strf(hz > 0 ? "This display runs at %.2f Hz, not within 1%% of 60 or a multiple: the exact speed is used."
                                                  : "This display's refresh rate isn't known: the exact speed is used.", hz).c_str());
        }
    } else {
        ImGui::TextDisabled("Without vsync the game always runs at the cabinet's 60.1 frames a second.");
    }
    if (ImGui::Button("Look at the picture")) peek_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(any key or button comes back)");

    ImGui::SeparatorText("Graphics device");
    const auto& devices = renderer_.devices();
    const std::string label = d.gpu.empty() ? "The best one" : d.gpu;
    if (ImGui::BeginCombo("Device", label.c_str())) {
        if (ImGui::Selectable("The best one", d.gpu.empty())) d.gpu.clear();
        for (const auto& name : devices)
            if (ImGui::Selectable(name.c_str(), d.gpu == name)) d.gpu = name;
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("In use: %s. A change takes effect the next time the game starts.", renderer_.device_name().c_str());
}

void Menu::game_tab() {
    ImGui::SeparatorText("The cabinet");
    ImGui::Checkbox("Self-test switch (the cabinet's test mode; F2)", &input_.test_switch);

    ImGui::SeparatorText("About");
    ImGui::TextWrapped("APB QilinRecomp %s: a static recompilation, the game's own code compiled for this computer.", QILIN_APP_VERSION);
    ImGui::TextWrapped("APB: Atari Games, 1987. Graphics: Vulkan. Window, sound and input: SDL3 (zlib). Menu: Dear ImGui (MIT).");
    ImGui::TextWrapped("Settings file: %s", path_.c_str());
    const auto folder = std::filesystem::path(std::u8string(path_.begin(), path_.end())).parent_path().u8string();
    ImGui::TextWrapped("Saves (high scores and audits): %s", std::string(folder.begin(), folder.end()).c_str());
}

}  // namespace qilin::app
