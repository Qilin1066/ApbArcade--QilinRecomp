#include "menu.hpp"
#include "text.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace qilin::app {

namespace {

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
    ImGui::GetStyle().ScaleAllSizes(1.25f);
    ImGui::GetStyle().FontScaleMain = 1.25f;
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
    input_.playing = !open;
    input_.stop_listening();
    listening_button_ = -1;
    finding_wheel_ = false;
    measuring_ = false;
    SDL_SetWindowRelativeMouseMode(window_, !open && input_.settings.steering == Steering::mouse);
    if (!open) save();   // closing the menu keeps what was set
}

void Menu::save() {
    if (!input_.settings.save(path_)) std::fprintf(stderr, "apb: couldn't write the settings to %s\n", path_.c_str());
}

void Menu::handle(const SDL_Event& e) {
    if (ok_ && (open_ || SDL_GetTicks() < hint_until_)) ImGui_ImplSDL3_ProcessEvent(&e);
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat) break;
        if (e.key.key == SDLK_F1) toggle();
        // Escape opens the menu, or closes it (unless a control is being set:
        // then it gives up waiting for one).
        if (e.key.key == SDLK_ESCAPE) {
            if (open_ && (input_.listening() != Input::Listen::none || measuring_)) {
                input_.stop_listening();
                listening_button_ = -1;
                finding_wheel_ = false;
                measuring_ = false;
            } else {
                toggle();
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // Start (the game has no use for it) or Guide; not Back, the gamepad's coin.
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START || e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) toggle();
        break;
    default: break;
    }
}

void Menu::frame() {
    const bool hint = !open_ && SDL_GetTicks() < hint_until_;
    if (!ok_ || (!open_ && !hint)) {
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
    if (hint) {
        ImGui::SetNextWindowPos(ImVec2(view->WorkPos.x + 12, view->WorkPos.y + view->WorkSize.y - 12), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextUnformatted("F1 or Esc (gamepad: Start): settings");
        ImGui::End();
    } else {
        ImGui::SetNextWindowPos(view->WorkPos);
        ImGui::SetNextWindowSize(view->WorkSize);
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::Begin("APB settings", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                                  ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);   // sliders take half; their labels the rest
        ImGui::TextDisabled("Paused. F1 or Esc to go back to the game; settings are saved when you do.");
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Controls")) {
                controls_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Game")) {
                game_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopItemWidth();
        ImGui::End();
    }
    ImGui::Render();
}

void Menu::controls_tab() {
    if (ImGui::CollapsingHeader("Connected controllers", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (input_.devices().empty()) ImGui::TextDisabled("None: the keyboard only.");
        for (const auto& d : input_.devices()) {
            ImGui::BulletText("%s  (%s; %d axes, %d buttons%s)", d.name.c_str(), Input::type_name(d.type), SDL_GetNumJoystickAxes(d.joystick),
                              SDL_GetNumJoystickButtons(d.joystick), SDL_IsJoystickHaptic(d.joystick) ? "; force feedback" : "");
        }
    }
    steering_section();
    pedal_section();
    buttons_section();
    ImGui::Separator();
    if (ImGui::Button("Reset all controls to the defaults")) {
        input_.settings = Settings{};
        input_.configure();
    }
}

void Menu::steering_section() {
    if (!ImGui::CollapsingHeader("Steering", ImGuiTreeNodeFlags_DefaultOpen)) return;
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
        s.steering = static_cast<Steering>(mode);
        input_.configure();
    }

    if (s.steering == Steering::wheel) {
        ImGui::Text("Wheel: %s", s.wheel_axis.describe().c_str());
        ImGui::SameLine();
        if (input_.listening() == Input::Listen::wheel) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Turn the wheel...");
        } else if (ImGui::Button("Find it")) {
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
        changed |= ImGui::SliderFloat("Rotation, lock to lock (degrees)", &rotation, 180, 3600, "%.0f");
        ImGui::TextDisabled("Set it to what the wheel's own software uses (Moza Pit House, Logitech G HUB, ...), or measure it:");
        for (const int preset : {540, 900, 1080, 1440, 2160}) {
            ImGui::SameLine();
            if (ImGui::SmallButton(std::to_string(preset).c_str())) {
                rotation = static_cast<float>(preset);
                changed = true;
            }
        }
        if (!s.wheel_axis.empty()) {
            if (!measuring_) {
                if (ImGui::Button("Measure: turn the wheel one full turn")) {
                    measuring_ = true;
                    measure_from_ = input_.read(s.wheel_axis);
                }
            } else {
                const double moved = input_.read(s.wheel_axis) - measure_from_;
                ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Turn the wheel exactly one full turn (360 degrees), then press Done. Moved so far: %.3f", moved);
                if (ImGui::Button("Done") && std::abs(moved) > 0.05) {
                    rotation = static_cast<float>(std::clamp(720.0 / std::abs(moved), 180.0, 10000.0));
                    changed = true;
                    measuring_ = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) measuring_ = false;
            }
        }
        float counts = static_cast<float>(s.wheel.counts_per_turn);
        changed |= ImGui::SliderFloat("Sensitivity (dial counts per turn)", &counts, 32, 512, "%.0f");
        bool edge = s.wheel.edge < 1.0;
        changed |= ImGui::Checkbox("Keep turning while the wheel is held at its lock (for wheels that stop)", &edge);
        // (Unticked, the edge is 1; ticking it again starts from the default.)
        float edge_start = static_cast<float>(s.wheel.edge < 1.0 ? s.wheel.edge : controls::WheelSettings{}.edge);
        float edge_rate = static_cast<float>(s.wheel.edge_rate);
        if (edge) {
            changed |= ImGui::SliderFloat("Starts this far to the lock", &edge_start, 0.6f, 0.99f, "%.2f");
            changed |= ImGui::SliderFloat("Turning speed at the lock (counts a frame)", &edge_rate, 1, 16, "%.1f");
        }
        changed |= ImGui::Checkbox("Reverse the wheel", &s.wheel.invert);
        if (changed) {
            s.wheel.rotation = rotation;
            s.wheel.counts_per_turn = counts;
            s.wheel.edge = edge ? edge_start : 1.0;
            s.wheel.edge_rate = edge_rate;
            input_.configure();
        }
        if (ImGui::SliderInt("Force feedback: friction (%)", &s.friction, 0, 100)) input_.configure();
        ImGui::TextDisabled("No centring spring: like the cabinet's, the wheel turns freely; friction keeps it from spinning on.");
    } else if (s.steering == Steering::mouse) {
        float m = static_cast<float>(s.mouse_counts);
        if (ImGui::SliderFloat("Dial counts per mouse step", &m, 0.02f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) s.mouse_counts = m;
        ImGui::TextDisabled("While playing, the mouse is captured; the menu frees it.");
    }
    float stick = static_cast<float>(s.stick_counts);
    if (ImGui::SliderFloat("Stick and keys: turning speed (counts a frame at full)", &stick, 1, 16, "%.1f")) {
        s.stick_counts = stick;
        input_.configure();
    }

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
    if (!ImGui::CollapsingHeader("Gas pedal", ImGuiTreeNodeFlags_DefaultOpen)) return;
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
    if (!ImGui::CollapsingHeader("Buttons and coins", ImGuiTreeNodeFlags_DefaultOpen)) return;
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

void Menu::game_tab() {
    ImGui::Checkbox("Self-test switch (the cabinet's test mode; F2)", &input_.test_switch);
    ImGui::Separator();
    ImGui::Text("Settings file: %s", path_.c_str());
    ImGui::TextDisabled("APB (Atari Games, 1987), recompiled by ApbArcade-QilinComp. Menu: Dear ImGui (MIT).");
    ImGui::Separator();
    if (ImGui::Button("Quit the game")) quit_ = true;
}

}  // namespace qilin::app
