// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <span>
#include <utility>
#ifndef MSR_NATIVE
#include <QSettings>
#endif
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#ifdef MSR_OPENGL
#include <backends/imgui_impl_opengl3.h>
#endif
#include "common/logging/log.h"
#include "msr/button_glyphs.h"
#include "msr/cheats.h"
#include "msr/frame_rate.h"
#include "msr/overlay.h"
#include "msr/presentation.h"

namespace MSR {
namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct LoaderData {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
};

PFN_vkVoidFunction LoadVulkanFunction(const char* name, void* user_data) {
    const auto* data = static_cast<const LoaderData*>(user_data);
    return data->get_instance_proc_addr(data->instance, name);
}

void CheckResult(VkResult result) {
    if (result != VK_SUCCESS) {
        LOG_ERROR(Frontend, "Settings overlay Vulkan error {}", static_cast<int>(result));
    }
}

/// A control reminder: the game's buttons and the port's controls as tokens ({A}, {Touch}...),
/// named for the device in use (msr/button_glyphs.h Keys: the pad's labels or the keyboard's keys;
/// in the emulator-based build the 3DS names as the game's icons show them, and both devices').
struct ControlHint {
    const char* keys;
    const char* action;
};

// The game offers the map tools, file copy and delete only by touch (its buttons do nothing
// there); touch mode reaches them.
constexpr ControlHint PauseMapHints[] = {
    {"{R}", "Samus page"},
    {"{B}", "Back to the game"},
    {"{Touch}", "Touch mode: scan, map pins, info, settings"},
};
constexpr ControlHint PauseSamusHints[] = {
    {"{Stick}", "Choose an item"},
    {"{A}", "Read more"},
    {"{L}", "Map"},
    {"{B}", "Back to the game"},
};
constexpr ControlHint MapPinsHints[] = {
    {"{Touch}", "Touch mode: move the pins"},
    {"{B}", "Back to the map"},
};
constexpr ControlHint FileListHints[] = {
    {"{A}", "Open a file"},
    {"{Touch}", "Touch mode: copy or delete an open file"},
};
constexpr ControlHint FileOpenHints[] = {
    {"{Touch}", "Touch mode: copy or delete"},
    {"{B}", "Back to the files"},
};
constexpr ControlHint TouchHints[] = {
    {"{Move}", "Move"},
    {"{Tap}", "Tap; hold to drag"},
    {"{Leave}", "Leave touch mode"},
};
constexpr ControlHint MapPinsTouchHints[] = {
    {"{Move}", "Move"},
    {"Up", "The pins on the map (down: the buttons again)"},
    {"Hold {Tap} on a pin", "Drag it to the map, or to the bin to remove it"},
    {"{Leave}", "Leave touch mode"},
};
constexpr std::span<const ControlHint> HintSets[] = {
    PauseMapHints, PauseSamusHints, MapPinsHints, FileListHints,
    FileOpenHints, TouchHints,      MapPinsTouchHints,
};
enum HintSet { PauseMap, PauseSamus, MapPins, FileList, FileOpen, Touch, MapPinsTouch };

/// Dark style with the cyan of Samus Returns' own interface as the accent.
void ApplyStyle() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 6.f;
    style.FrameRounding = 4.f;
    style.GrabRounding = 4.f;
    style.TabRounding = 4.f;
    style.WindowBorderSize = 1.f;
    style.WindowPadding = {18.f, 14.f};
    style.FramePadding = {10.f, 6.f};
    style.ItemSpacing = {10.f, 9.f};
    auto* colors = style.Colors;
    const ImVec4 accent{.24f, .66f, .86f, 1.f};
    const ImVec4 accent_dim{.16f, .42f, .56f, 1.f};
    colors[ImGuiCol_WindowBg] = {.03f, .07f, .11f, .94f};
    colors[ImGuiCol_Border] = {.24f, .66f, .86f, .55f};
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_TitleBgActive] = {.05f, .14f, .21f, 1.f};
    colors[ImGuiCol_FrameBg] = {.08f, .17f, .25f, 1.f};
    colors[ImGuiCol_FrameBgHovered] = colors[ImGuiCol_FrameBgActive] = accent_dim;
    colors[ImGuiCol_Button] = {.08f, .2f, .3f, 1.f};
    colors[ImGuiCol_ButtonHovered] = accent_dim;
    colors[ImGuiCol_ButtonActive] = accent;
    colors[ImGuiCol_Tab] = {.06f, .15f, .22f, 1.f};
    colors[ImGuiCol_TabHovered] = accent_dim;
    colors[ImGuiCol_TabSelected] = {.12f, .34f, .48f, 1.f};
    colors[ImGuiCol_SliderGrab] = colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrabActive] = {.5f, .85f, 1.f, 1.f};
    colors[ImGuiCol_Header] = colors[ImGuiCol_HeaderHovered] = accent_dim;
    colors[ImGuiCol_NavCursor] = {.6f, .9f, 1.f, 1.f};
    colors[ImGuiCol_PlotHistogram] = accent;   // the launcher's build progress
}

/// A row's label, wrapped before `column` (window local) so no font runs it under the row's
/// control, which follows at `column`.
void Label(const char* text, float column) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(column - ImGui::GetStyle().ItemSpacing.x);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::SameLine(column);
}

/// Explanatory text, dimmed and wrapped to the width available.
void Note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

/// The height Note(text) takes, spacing included.
float NoteHeight(const char* text) {
    return ImGui::CalcTextSize(text, nullptr, false, ImGui::GetContentRegionAvail().x).y +
           ImGui::GetStyle().ItemSpacing.y;
}

/// A video preset's internal resolution, within what the menu offers.
int PresetResolution(const VideoPreset& preset, const EmulatorSettings& e) {
    return std::clamp(preset.resolution, 1, std::max(e.max_resolution, 1));
}

/// The video preset (VideoPresets) the Display tab's settings make, or -1: Custom.
int VideoPresetOf(const PlayerSettings& p, const EmulatorSettings& e) {
    for (std::size_t i = 0; i < VideoPresets.size(); ++i) {
        const auto& preset = VideoPresets[i];
        const bool rate = preset.frame_rate <= 30 ? p.frame_rate == preset.frame_rate
                                                  : p.frame_rate >= preset.frame_rate;
        // power-saving waits count only for the preset that turns them on (Minimum); the others leave them
        if (rate && e.resolution == PresetResolution(preset, e) &&
            (!e.aniso_option || e.aniso == preset.aniso) &&
            (!e.performance_options ||
             (e.fsr == preset.fsr && p.wide_culling != preset.only_3ds_view &&
              (!preset.power_saving || e.power_saving)))) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ApplyVideoPreset(const VideoPreset& preset, PlayerSettings& p, EmulatorSettings& e) {
    e.resolution = PresetResolution(preset, e);
    if (e.aniso_option) {
        e.aniso = preset.aniso;
    }
    if (e.performance_options) {
        e.fsr = preset.fsr;
        p.wide_culling = !preset.only_3ds_view;
        e.power_saving = e.power_saving || preset.power_saving;
    }
    if (preset.frame_rate <= 30 || p.frame_rate < preset.frame_rate) {
        p.frame_rate = preset.frame_rate;
    }
}

/// What a video preset sets, for the note under the Video preset row.
std::string VideoPresetNote(const VideoPreset& preset, const EmulatorSettings& e) {
    std::string note = std::string(ResolutionNames[PresetResolution(preset, e)]) + " internal resolution";
    if (e.aniso_option) {
        const auto aniso = std::find(AnisoChoices.begin(), AnisoChoices.end(), preset.aniso);
        note += preset.aniso > 1 ? std::string(", ") + AnisoNames[aniso - AnisoChoices.begin()] +
                                       " anisotropic filtering"
                                 : ", no anisotropic filtering";
    }
    if (e.performance_options) {
        note += preset.fsr ? ", FSR 1 upscaling" : ", linear upscaling";
    }
    note += preset.frame_rate <= 30 ? ", 30 FPS (the original)"
                                    : ", " + std::to_string(preset.frame_rate) +
                                          " FPS (a faster rate you chose stays)";
    if (e.performance_options && preset.only_3ds_view) {
        note += preset.power_saving ? "; only the 3DS's view drawn, power-saving waits (below)"
                                    : "; only the 3DS's view drawn (below)";
    }
    return note + ".";
}

} // namespace

#ifndef MSR_NATIVE
void PlayerSettings::Load(const QString& path) {
    QSettings file(path, QSettings::IniFormat);
    widescreen = file.value(QStringLiteral("widescreen"), widescreen).toBool();
    ultrawide = file.value(QStringLiteral("ultrawide"), ultrawide).toBool() && widescreen;   // never on without 16:9
    camera_zoom = std::clamp(file.value(QStringLiteral("camera_zoom"), camera_zoom).toFloat(),
                             CameraZoomMin, CameraZoomMax);
    hud_scale = file.value(QStringLiteral("hud_scale"), hud_scale).toFloat();
    hud_opacity = file.value(QStringLiteral("hud_opacity"), hud_opacity).toFloat();
    show_minimap = file.value(QStringLiteral("show_minimap"), show_minimap).toBool();
    minimap_size = file.value(QStringLiteral("minimap_size"), minimap_size).toFloat();
    minimap_opacity = file.value(QStringLiteral("minimap_opacity"), minimap_opacity).toFloat();
    show_counters = file.value(QStringLiteral("show_counters"), show_counters).toBool();
    show_abilities = file.value(QStringLiteral("show_abilities"), show_abilities).toBool();
    hud_within_16_9 = file.value(QStringLiteral("hud_within_16_9"), hud_within_16_9).toBool();
    panel_scale = file.value(QStringLiteral("panel_scale"), panel_scale).toFloat();
    menu_backdrop = file.value(QStringLiteral("menu_backdrop"), menu_backdrop).toFloat();
    control_hints = file.value(QStringLiteral("control_hints"), control_hints).toBool();
    menu_scale = file.value(QStringLiteral("menu_scale"), menu_scale).toFloat();
    show_fps = file.value(QStringLiteral("show_fps"), show_fps).toBool();
    frame_rate = file.value(QStringLiteral("frame_rate"), frame_rate).toInt();
    bloom = std::clamp(file.value(QStringLiteral("bloom"), bloom).toFloat(), 0.f, 1.f);
    hdr = file.value(QStringLiteral("hdr"), hdr).toBool();
    hdr_peak = file.value(QStringLiteral("hdr_peak"), hdr_peak).toFloat();
    hdr_paper = file.value(QStringLiteral("hdr_paper"), hdr_paper).toFloat();
    fxaa = file.value(QStringLiteral("fxaa"), fxaa).toBool();
    smooth_packs = file.value(QStringLiteral("smooth_packs"), smooth_packs).toBool();
    brightness = std::clamp(file.value(QStringLiteral("brightness"), brightness).toFloat(), .5f, 1.5f);
    contrast = std::clamp(file.value(QStringLiteral("contrast"), contrast).toFloat(), .5f, 1.5f);
    saturation = std::clamp(file.value(QStringLiteral("saturation"), saturation).toFloat(), .5f, 1.5f);
    if (std::find(FrameRate::Choices.begin(), FrameRate::Choices.end(), frame_rate) ==
        FrameRate::Choices.end()) {
        frame_rate = 60;
    }
    unlock_galleries = file.value(QStringLiteral("unlock_galleries"), unlock_galleries).toBool();
    unlock_fusion_mode =
        file.value(QStringLiteral("unlock_fusion_mode"), unlock_fusion_mode).toBool();
    unlock_hard_mode = file.value(QStringLiteral("unlock_hard_mode"), unlock_hard_mode).toBool();
    fusion_suit = file.value(QStringLiteral("fusion_suit"), fusion_suit).toBool();
    infinite_energy = file.value(QStringLiteral("infinite_energy"), infinite_energy).toBool();
    infinite_missiles = file.value(QStringLiteral("infinite_missiles"), infinite_missiles).toBool();
    infinite_aeion = file.value(QStringLiteral("infinite_aeion"), infinite_aeion).toBool();
}

void PlayerSettings::Save(const QString& path) const {
    QSettings file(path, QSettings::IniFormat);
    file.setValue(QStringLiteral("widescreen"), widescreen);
    file.setValue(QStringLiteral("ultrawide"), ultrawide);
    file.setValue(QStringLiteral("camera_zoom"), camera_zoom);
    file.setValue(QStringLiteral("hud_scale"), hud_scale);
    file.setValue(QStringLiteral("hud_opacity"), hud_opacity);
    file.setValue(QStringLiteral("show_minimap"), show_minimap);
    file.setValue(QStringLiteral("minimap_size"), minimap_size);
    file.setValue(QStringLiteral("minimap_opacity"), minimap_opacity);
    file.setValue(QStringLiteral("show_counters"), show_counters);
    file.setValue(QStringLiteral("show_abilities"), show_abilities);
    file.setValue(QStringLiteral("hud_within_16_9"), hud_within_16_9);
    file.setValue(QStringLiteral("panel_scale"), panel_scale);
    file.setValue(QStringLiteral("menu_backdrop"), menu_backdrop);
    file.setValue(QStringLiteral("control_hints"), control_hints);
    file.setValue(QStringLiteral("menu_scale"), menu_scale);
    file.setValue(QStringLiteral("show_fps"), show_fps);
    file.setValue(QStringLiteral("frame_rate"), frame_rate);
    file.setValue(QStringLiteral("bloom"), bloom);
    file.setValue(QStringLiteral("hdr"), hdr);
    file.setValue(QStringLiteral("hdr_peak"), hdr_peak);
    file.setValue(QStringLiteral("hdr_paper"), hdr_paper);
    file.setValue(QStringLiteral("fxaa"), fxaa);
    file.setValue(QStringLiteral("smooth_packs"), smooth_packs);
    file.setValue(QStringLiteral("brightness"), brightness);
    file.setValue(QStringLiteral("contrast"), contrast);
    file.setValue(QStringLiteral("saturation"), saturation);
    file.setValue(QStringLiteral("unlock_galleries"), unlock_galleries);
    file.setValue(QStringLiteral("unlock_fusion_mode"), unlock_fusion_mode);
    file.setValue(QStringLiteral("unlock_hard_mode"), unlock_hard_mode);
    file.setValue(QStringLiteral("fusion_suit"), fusion_suit);
    file.setValue(QStringLiteral("infinite_energy"), infinite_energy);
    file.setValue(QStringLiteral("infinite_missiles"), infinite_missiles);
    file.setValue(QStringLiteral("infinite_aeion"), infinite_aeion);
}
#endif

void PlayerSettings::Publish() const {
    namespace P = Presentation;
    P::widescreen = widescreen;
    P::ultrawide = ultrawide;
    P::fill_16_10 = fill_16_10;
    P::zoom = camera_zoom;
    P::wide_culling = wide_culling;
    P::hud_scale = hud_scale;
    P::hud_opacity = hud_opacity;
    P::show_minimap = show_minimap;
    P::minimap_size = minimap_size;
    P::minimap_opacity = minimap_opacity;
    P::show_counters = show_counters;
    P::show_abilities = show_abilities;
    P::hud_within_16_9 = hud_within_16_9;
    P::panel_scale = panel_scale;
    P::menu_backdrop = menu_backdrop;
    P::bloom = bloom;
    P::hdr = hdr;
    P::hdr_peak = hdr_peak;
    P::hdr_paper = hdr_paper;
    P::fxaa = fxaa;
    P::smooth_packs = smooth_packs;
    P::brightness = brightness;
    P::contrast = contrast;
    P::saturation = saturation;
    FrameRate::target = frame_rate;
    Cheats::enabled = (unlock_galleries ? Cheats::Galleries : 0u) |
                      (unlock_fusion_mode ? Cheats::FusionMode : 0u) |
                      (unlock_hard_mode ? Cheats::HardMode : 0u) |
                      (fusion_suit ? Cheats::FusionSuit : 0u) |
                      (infinite_energy ? Cheats::InfiniteEnergy : 0u) |
                      (infinite_missiles ? Cheats::InfiniteMissiles : 0u) |
                      (infinite_aeion ? Cheats::InfiniteAeion : 0u);
}

Overlay::Overlay() {
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // The game's layout, Nintendo's: its A is the right face button (Xbox B) and chooses, its B
    // the bottom one (Xbox A) and goes back. msr-native gives the pad's choosing button as the
    // right one (overlay_host.cpp PadKeyOf: an Xbox or PlayStation pad chooses with the bottom one).
    io.ConfigNavSwapGamepadButtons = true;
    io.ConfigNavCursorVisibleAuto = true;
    ApplyStyle();

    // A large font, scaled down per frame, stays sharp from 720p to 4K. Linux distributions keep
    // the same fonts in different folders: SteamOS and Arch, Debian and Ubuntu, then Fedora.
    const char* const fonts[] = {"C:/Windows/Fonts/segoeui.ttf",
                                 "C:/Windows/Fonts/arial.ttf",
                                 "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                 "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                 "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
                                 "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                 "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    ImFont* font = nullptr;
    for (const char* path : fonts) {
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            font = io.Fonts->AddFontFromFileTTF(path, font_size);
            if (font) {
                break;
            }
        }
    }
    if (!font) {
        ImFontConfig config;
        config.SizePixels = font_size;
        io.Fonts->AddFontDefault(&config);
    }
}

Overlay::~Overlay() {
    if (context) {
        ImGui::DestroyContext(context);
    }
}

void Overlay::SetVisible(bool show) {
    if (!show && launcher_active.load()) {
        return;   // the launcher is this menu: it stays open
    }
    visible = show;
    // Input goes back to the game only once the closing button is released (InputBridge).
    if (show) {
        Presentation::block_game_input = true;
    }
    if (!show) {
        // Persist anything changed while the menu was open.
        Act(OverlayAction::Kind::Player);
    }
}

void Overlay::ShowStartupHint() {
    hint_started = Now();
}

void Overlay::SetEmulatorSettings(const EmulatorSettings& settings) {
    std::scoped_lock lock{mutex};
    emulator = settings;
}

void Overlay::SetPlayerSettings(const PlayerSettings& settings) {
    std::scoped_lock lock{mutex};
    player = settings;
    player.Publish();
}

PlayerSettings Overlay::GetPlayerSettings() {
    std::scoped_lock lock{mutex};
    return player;
}

void Overlay::Push(const Event& event) {
    std::scoped_lock lock{mutex};
    events.push_back(event);
}

void Overlay::Key(int key, bool down) {
    Push({Event::Type::Key, key, down, 0.f, 0.f});
}

void Overlay::Analog(int key, bool down, float value) {
    Push({Event::Type::Analog, key, down, value, 0.f});
}

void Overlay::MousePosition(float x, float y) {
    Push({Event::Type::Position, 0, false, x, y});
}

void Overlay::MouseButton(int button, bool down) {
    Push({Event::Type::Button, button, down, 0.f, 0.f});
}

void Overlay::MouseWheel(float delta) {
    Push({Event::Type::Wheel, 0, false, 0.f, delta});
}

std::vector<OverlayAction> Overlay::TakeActions() {
    std::scoped_lock lock{mutex};
    return std::exchange(actions, {});
}

void Overlay::Act(OverlayAction::Kind kind, int value, std::string text) {
    std::scoped_lock lock{mutex};
    actions.push_back({kind, emulator, value, std::move(text)});
}

void Overlay::SetLauncher(const LauncherSnapshot& snapshot) {
    {
        std::scoped_lock lock{mutex};
        launcher = snapshot;
    }
    launcher_active = snapshot.active;
    if (snapshot.active) {
        visible = true;
    }
}

void Overlay::SetControls(const ControlsSnapshot& snapshot) {
    std::scoped_lock lock{mutex};
    controls = snapshot;
}

void Overlay::SetMods(const ModsSnapshot& snapshot) {
    std::scoped_lock lock{mutex};
    mods = snapshot;
}

void Overlay::PlayerChanged(bool layout) {
    std::scoped_lock lock{mutex};
    player.Publish();
    if (layout) {
        actions.push_back({OverlayAction::Kind::Layout, emulator});
    }
}

void Overlay::EmulatorChanged() {
    std::scoped_lock lock{mutex};
    actions.push_back({OverlayAction::Kind::Emulator, emulator});
}

void Overlay::Init(const OverlayHook::VulkanContext& vulkan) {
    ImGui::SetCurrentContext(context);
    LoaderData loader{vulkan.instance, vulkan.get_instance_proc_addr};
    if (!ImGui_ImplVulkan_LoadFunctions(vulkan.api_version, LoadVulkanFunction, &loader)) {
        LOG_ERROR(Frontend, "Settings overlay: Vulkan functions unavailable");
        return;
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = vulkan.api_version;
    info.Instance = vulkan.instance;
    info.PhysicalDevice = vulkan.physical_device;
    info.Device = vulkan.device;
    info.QueueFamily = vulkan.queue_family;
    info.Queue = vulkan.queue;
    info.RenderPass = vulkan.render_pass;
    if (!vulkan.render_pass) {
        // msr-native draws its presentation with dynamic rendering
        info.UseDynamicRendering = true;
        info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &vulkan.color_format;
    }
    info.MinImageCount = 2;
    // One spare vertex buffer set beyond the frames the presenter keeps in flight.
    info.ImageCount = std::max(3u, vulkan.image_count + 1);
    info.DescriptorPoolSize = 8;
    info.CheckVkResultFn = CheckResult;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LOG_ERROR(Frontend, "Settings overlay: initialization failed");
        return;
    }
    {
        // Upload the font now; NewFrame would otherwise submit from inside command recording.
        std::scoped_lock lock{*vulkan.queue_mutex};
        ImGui_ImplVulkan_CreateFontsTexture();
    }
    backend_ready = true;
    previous_time = Now();
}

void Overlay::InitOpenGL() {
#ifdef MSR_OPENGL
    ImGui::SetCurrentContext(context);
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        LOG_ERROR(Frontend, "Settings overlay: OpenGL initialization failed");
        return;
    }
    opengl = true;
    backend_ready = true;
    previous_time = Now();
#endif
}

void Overlay::Shutdown() {
    if (!backend_ready) {
        return;
    }
    ImGui::SetCurrentContext(context);
#ifdef MSR_OPENGL
    if (opengl) {
        ImGui_ImplOpenGL3_Shutdown();
        opengl = backend_ready = false;
        return;
    }
#endif
    ImGui_ImplVulkan_Shutdown();
    backend_ready = false;
}

void Overlay::Draw(VkCommandBuffer command, std::uint32_t width, std::uint32_t height) {
    if (!backend_ready || opengl) {
        return;
    }
    BuildFrame(width, height, ImGui_ImplVulkan_NewFrame);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
}

void Overlay::DrawOpenGL(std::uint32_t width, std::uint32_t height) {
#ifdef MSR_OPENGL
    if (!backend_ready || !opengl) {
        return;
    }
    BuildFrame(width, height, ImGui_ImplOpenGL3_NewFrame);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#else
    (void)width;
    (void)height;
#endif
}

void Overlay::BuildFrame(std::uint32_t width, std::uint32_t height, void (*new_frame)()) {
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(width), static_cast<float>(height)};
    const double now = Now();
    io.DeltaTime = static_cast<float>(std::clamp(now - previous_time, 1e-5, .25));
    previous_time = now;

    // The game's own frames: the display refreshes at least twice per frame at 30 FPS.
    const std::uint64_t frames = FrameRate::game_frames.load(std::memory_order_relaxed);
    if (now - fps_started >= 1.0) {
        fps = static_cast<float>(static_cast<double>(frames - fps_frames) / (now - fps_started));
        fps_frames = frames;
        fps_started = now;
    }

    const bool show = visible.load();
    {
        std::scoped_lock lock{mutex};
        for (const auto& event : events) {
            switch (event.type) {
            case Event::Type::Key:
                io.AddKeyEvent(static_cast<ImGuiKey>(event.code), event.down);
                break;
            case Event::Type::Analog:
                io.AddKeyAnalogEvent(static_cast<ImGuiKey>(event.code), event.down, event.x);
                break;
            case Event::Type::Position:
                io.AddMousePosEvent(event.x, event.y);
                break;
            case Event::Type::Button:
                io.AddMouseButtonEvent(event.code, event.down);
                break;
            case Event::Type::Wheel:
                io.AddMouseWheelEvent(0.f, event.y);
                break;
            }
        }
        events.clear();
    }
    if (!show) {
        io.ClearInputKeys();
        io.AddMouseButtonEvent(0, false);
    }

    // Sizes follow the view: designed at 1080p, scaled by the menu size setting, and small enough
    // for the 780 x 640 menu to fit in the view with a margin.
    const float scale =
        std::min(std::clamp(static_cast<float>(height) / 1080.f, .6f, 3.f) *
                     GetPlayerSettings().menu_scale,
                 std::min(static_cast<float>(width) / 820.f, static_cast<float>(height) / 680.f));
    static const ImGuiStyle base_style = ImGui::GetStyle();
    ImGui::GetStyle() = base_style;
    ImGui::GetStyle().ScaleAllSizes(scale);
    io.FontGlobalScale = 24.f * scale / font_size;

    new_frame();
    ImGui::NewFrame();
    const bool launching = launcher_active.load();   // the launcher: no game behind the menu
    if (!launching) {
        DrawHint(scale, static_cast<float>(width), static_cast<float>(height));
    }
    if (!show) {
        DrawControlHints(static_cast<float>(width), static_cast<float>(height));
    }
    if (GetPlayerSettings().show_fps && !launching) {
        DrawFps(scale, static_cast<float>(width));
    }
    if (show) {
        if (!was_visible) {
            ImGui::SetNextWindowFocus();
        }
        DrawMenu(scale, !was_visible);
    }
    was_visible = show;
    ImGui::Render();
}

void Overlay::DrawHint(float scale, float width, float height) {
    const double started = hint_started.load();
    if (started < 0.0) {
        return;
    }
    // Hold while the title comes up, then fade out over two seconds, as in DekUI.
    const float t = static_cast<float>(previous_time - started);
    const float fade = std::clamp((t - 7.f) / 2.f, 0.f, 1.f);
    const float alpha = 1.f - fade * fade * (3.f - 2.f * fade);
    if (alpha <= 0.f) {
        hint_started = -1.0;
        return;
    }
    const char* label = "Press F1 or Select for settings";
    const float size = 22.f * scale;
    const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, label);
    const float pad = 10.f * scale;
    const ImVec2 pos{18.f * scale + pad, height - 18.f * scale - pad - text.y};
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({pos.x - pad, pos.y - pad}, {pos.x + text.x + pad, pos.y + text.y + pad},
                        IM_COL32(4, 14, 22, static_cast<int>(190 * alpha)), 6.f * scale);
    draw->AddText(ImGui::GetFont(), size, pos, IM_COL32(220, 240, 255, static_cast<int>(255 * alpha)),
                  label);
    (void)width;
}

void Overlay::DrawControlHints(float width, float height) {
    namespace P = Presentation;
    // (not a menu beside the view: the game's own screen over play, whose buttons are not the pause menu's)
    const bool menu = static_cast<P::Bottom>(P::bottom.load()) == P::Bottom::Menu &&
                      P::panel_visible.load() && !P::menu_beside.load();
    const auto page = static_cast<P::Page>(P::page.load());
    // The menu's "Menu help text" option turns off every set, touch mode's too.
    const bool enabled = GetPlayerSettings().control_hints;
    int set = -1;
    if (enabled && P::touch_mode.load()) {
        set = menu && page == P::Page::MapPins ? MapPinsTouch : Touch;
    } else if (enabled && menu) {
        switch (page) {
        case P::Page::PauseMap:
            set = PauseMap;
            break;
        case P::Page::PauseSamus:
            set = PauseSamus;
            break;
        case P::Page::MapPins:
            set = MapPins;
            break;
        case P::Page::FileList:
            set = FileList;
            break;
        case P::Page::FileOpen:
            set = FileOpen;
            break;
        default:
            break;
        }
    }
    // Fade in and out with the panel, keeping the last set while fading out.
    const float step = ImGui::GetIO().DeltaTime * 6.f;
    hint_alpha = std::clamp(hint_alpha + (set >= 0 ? step : -step), 0.f, 1.f);
    if (set >= 0) {
        hint_set = set;
    }
    const float x1 = static_cast<float>(P::panel_x1.load());
    const float y1 = static_cast<float>(P::panel_y1.load());
    if (hint_alpha <= 0.f || hint_set < 0 || x1 <= 0.f) {
        return;
    }

    // A column beside the panel, sized by the view so it fits in the room the 4:3 panel leaves
    // in a 16:9 view; over the view's corner when the window has no room beside it.
    const float size = std::clamp(height * .026f, 14.f, 72.f);
    const float pad = size * .6f;
    const float gap = size * .5f;
    const float room = width - x1 - 2.f * gap;
    const bool beside = room >= size * 6.f;
    const float box_w = beside ? std::min(room, size * 13.f) : std::min(width - 2.f * gap, size * 15.f);
    const float text_w = box_w - 2.f * pad;
    auto* font = ImGui::GetFont();
    const auto hints = HintSets[hint_set];
    std::vector<std::string> keys;
    for (const auto& hint : hints) {
        keys.push_back(ButtonGlyphs::Keys(hint.keys));
    }
    float box_h = pad * 2.f;
    for (std::size_t i = 0; i < hints.size(); ++i) {
        box_h += font->CalcTextSizeA(size, FLT_MAX, text_w, keys[i].c_str()).y +
                 font->CalcTextSizeA(size, FLT_MAX, text_w, hints[i].action).y + size * .5f;
    }
    box_h -= size * .5f;
    const float x = beside ? x1 + gap : width - gap - box_w;
    const float y = std::min(y1, height) - gap - box_h;

    const auto alpha = [this](int value) { return static_cast<int>(value * hint_alpha); };
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({x, y}, {x + box_w, y + box_h}, IM_COL32(4, 14, 22, alpha(215)),
                        size * .3f);
    draw->AddRectFilled({x, y + size * .4f}, {x + size * .12f, y + box_h - size * .4f},
                        IM_COL32(60, 170, 220, alpha(255)));
    float line_y = y + pad;
    for (std::size_t i = 0; i < hints.size(); ++i) {
        draw->AddText(font, size, {x + pad, line_y}, IM_COL32(110, 205, 250, alpha(255)),
                      keys[i].c_str(), nullptr, text_w);
        line_y += font->CalcTextSizeA(size, FLT_MAX, text_w, keys[i].c_str()).y;
        draw->AddText(font, size, {x + pad, line_y}, IM_COL32(220, 240, 255, alpha(255)),
                      hints[i].action, nullptr, text_w);
        line_y += font->CalcTextSizeA(size, FLT_MAX, text_w, hints[i].action).y + size * .5f;
    }
}

void Overlay::DrawControls(float scale, float label_width,
                           const std::function<void()>& default_focus) {
    ControlsSnapshot snapshot;
    {
        std::scoped_lock lock{mutex};
        snapshot = controls;
    }
    ImGui::TextDisabled("Profile: %s", snapshot.profile.c_str());
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.f;
    ImGui::BeginDisabled(!snapshot.controller_connected);
    if (ImGui::Button("Gamepad layout", {half, 0.f})) {
        Act(OverlayAction::Kind::GamepadLayout);
    }
    ImGui::EndDisabled();
    default_focus();
    ImGui::SameLine();
    if (ImGui::Button("Keyboard layout", {half, 0.f})) {
        Act(OverlayAction::Kind::KeyboardLayout);
    }
    // msr-native: the Aeion abilities (the 3DS D-pad) on the right stick, the pad's D-pad moving
    // Samus as the left stick does
    if (snapshot.right_stick_option) {
        bool on = snapshot.right_stick_dpad;
        if (ImGui::Checkbox("Aeion abilities on the right stick (the D-pad moves)", &on)) {
            Act(OverlayAction::Kind::RightStickDpad, on ? 1 : 0);
        }
    }
    // msr-native: menus choose with the game's A as bound below (on: the 3DS's layout, A on the right), not with A /
    // Cross, the bottom button (off, the default)
    if (snapshot.menu_layout_option) {
        bool on = snapshot.menu_confirm_right;
        if (ImGui::Checkbox("Nintendo menu layout (the game's A confirms, as bound below)", &on)) {
            Act(OverlayAction::Kind::MenuConfirmRight, on ? 1 : 0);
        }
    }
    // msr-native (#35): the triggers are buttons like the others below; how far one is pulled before it counts. It shows
    // at once and is saved when let go.
    if (snapshot.trigger_option) {
        int press = snapshot.trigger_press;
        Label("Trigger press point", label_width);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##triggerpress", &press, 10, 90, "%d%% of its travel")) {
            std::scoped_lock lock{mutex};
            controls.trigger_press = press;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            Act(OverlayAction::Kind::TriggerPress, press);
        }
    }

    // The bindings scroll in their own region; the reference stays below them. msr-native names
    // the keys or buttons of the device in use (ButtonGlyphs::Keys) and aims with the mouse (video.c).
    const bool native = snapshot.profile == "msr-native";
    std::vector<std::string> reference;
    if (native) {
        reference = {
            "Mouse: the left button shoots, the right one held aims at the pointer, the wheel changes beams",
            ButtonGlyphs::Keys("Move: {Stick}        Beams: {Beams}        Morph Ball: {Morph}"),
            ButtonGlyphs::Keys("Settings: {Close}        Fullscreen: F11"),
            ButtonGlyphs::Keys("Touch navigation: {Touch} in menus"),
        };
        // a pad chooses and goes back in menus with its own layout's buttons (msr/glyphs.cpp), whatever A and B do in play
        const u32 dev = ButtonGlyphs::device.load();
        if (dev == ButtonGlyphs::Xbox || dev == ButtonGlyphs::PlayStation || dev == ButtonGlyphs::Nintendo) {
            reference.push_back(ButtonGlyphs::Keys(snapshot.menu_confirm_right
                ? "In menus: {Choose} chooses, {Back} goes back"
                : "In menus: {Choose} chooses, {Back} goes back (the rows above are for play)"));
        }
        if (snapshot.right_stick_dpad) {
            reference.push_back("Aeion abilities: the right stick        The D-pad moves, as the left stick");
        }
    } else {
        reference = {
            "Beams: ZR (right trigger)        Morph Ball: ZL (left trigger)",
            "Settings: F1 or Select        Fullscreen: F11",
            "Touch navigation: Tab, or L3 in menus",
        };
    }
    float reference_height = 0.f;
    for (const auto& line : reference) {
        reference_height += NoteHeight(line.c_str());
    }
    const float height =
        std::max(80.f * scale, ImGui::GetContentRegionAvail().y - reference_height);
    ImGui::BeginChild("##bindings", {0.f, height}, ImGuiChildFlags_NavFlattened);
    for (const auto& row : snapshot.rows) {
        ImGui::PushID(row.id);
        Label(row.label.c_str(), label_width);
        if (snapshot.capturing == row.id) {
            const char* prompt = row.id >= 100 ? "Move a stick...   (Esc cancels)"
                                               : "Press a button, trigger or key...   (Esc cancels)";
            // The same ID whatever the label, so focus stays on the row while it changes.
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button((std::string(prompt) + "###bind").c_str(), {-FLT_MIN, 0.f})) {
                Act(OverlayAction::Kind::CancelBind);
            }
            ImGui::PopStyleColor();
        } else if (ImGui::Button((row.binding + "###bind").c_str(), {-FLT_MIN, 0.f})) {
            Act(OverlayAction::Kind::Bind, row.id);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    for (const auto& line : reference) {
        Note(line.c_str());
    }
}

ModsSnapshot Overlay::ModsCopy() {
    std::scoped_lock lock{mutex};
    return mods;
}

bool Overlay::ModsTabShown() const {
    return was_visible && (tab == 4 || tab == 5);   // DrawMenu's Mods and Textures
}

void Overlay::ChangeMods(OverlayAction::Kind kind, int value,
                         const std::function<void(ModsSnapshot&)>& edit) {
    std::scoped_lock lock{mutex};
    edit(mods);
    actions.push_back({kind, emulator, value});
}

int Overlay::DrawModList(const ModsSnapshot& snapshot,
                         std::vector<ModsSnapshot::Entry> ModsSnapshot::*list,
                         OverlayAction::Kind toggle, OverlayAction::Kind up,
                         OverlayAction::Kind down, const std::function<void()>& default_focus) {
    const auto& entries = snapshot.*list;
    const int count = static_cast<int>(entries.size());
    for (int i = 0; i < count; ++i) {
        const auto& entry = entries[i];
        // By name, so the cursor stays with an entry as it moves.
        ImGui::PushID(entry.name.c_str());
        bool enabled = entry.enabled;
        if (ImGui::Checkbox(entry.name.c_str(), &enabled)) {
            ChangeMods(toggle, i,
                       [list, i, enabled](ModsSnapshot& m) { (m.*list)[i].enabled = enabled; });
        }
        if (i == 0) {
            default_focus();
        }
        // The details follow the name when they fit before the arrows, else go under it.
        const auto& style = ImGui::GetStyle();
        const float arrows_x =
            ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight() * 2.f - style.ItemSpacing.x;
        ImGui::SameLine();
        const float detail_end =
            ImGui::GetCursorPosX() + ImGui::CalcTextSize(entry.detail.c_str()).x;
        const bool beside = detail_end + style.ItemSpacing.x <= arrows_x;
        if (beside) {
            ImGui::TextDisabled("%s", entry.detail.c_str());
            ImGui::SameLine();
        }
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), arrows_x));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
            ChangeMods(up, i, [list, i](ModsSnapshot& m) {
                std::swap((m.*list)[i], (m.*list)[i - 1]);
            });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == count);
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
            ChangeMods(down, i, [list, i](ModsSnapshot& m) {
                std::swap((m.*list)[i], (m.*list)[i + 1]);
            });
        }
        ImGui::EndDisabled();
        if (!beside) {
            const float indent = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
            ImGui::Indent(indent);
            ImGui::PushTextWrapPos(arrows_x - style.ItemSpacing.x);
            Note(entry.detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Unindent(indent);
        }
        ImGui::PopID();
    }
    return count;
}

void Overlay::DrawModsFooter(const ModsSnapshot& snapshot, const char* note) {
    if (snapshot.restart_needed) {
        ImGui::TextColored({.53f, .89f, .6f, 1.f}, "Restart the game to load the changes.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart now")) {
            Act(OverlayAction::Kind::Restart);
            SetVisible(false);
        }
    } else {
        Note(note);
    }
}

void Overlay::DrawGameMods(const std::function<void()>& default_focus) {
    const ModsSnapshot snapshot = ModsCopy();
    Note("Game mods, as 3DS emulators take them: romfs, exefs, code patches. The first one listed "
         "wins where mods overlap.");
    // Below the list: the folder button and a line about restarting.
    const char* const mods_note = "Mods load when the game starts.";
    const float footer =
        ImGui::GetFrameHeightWithSpacing() +
        (snapshot.restart_needed ? ImGui::GetTextLineHeightWithSpacing() : NoteHeight(mods_note));
    const float height = std::max(ImGui::GetFrameHeightWithSpacing() * 2.f,
                                  ImGui::GetContentRegionAvail().y - footer);
    ImGui::BeginChild("##mods", {0.f, height}, ImGuiChildFlags_NavFlattened);
    if (snapshot.mods.empty()) {
        ImGui::TextWrapped("No mods yet. Put each mod in its own folder in %s",
                           snapshot.mod_folder.c_str());
    }
    const int count = DrawModList(snapshot, &ModsSnapshot::mods, OverlayAction::Kind::GameMod,
                                  OverlayAction::Kind::GameModUp, OverlayAction::Kind::GameModDown,
                                  default_focus);
    ImGui::EndChild();

#ifndef __ANDROID__   // msr-native's Android app: the folder above, reached from a PC over USB
    if (ImGui::Button("Open the mods folder", {-FLT_MIN, 0.f})) {
        Act(OverlayAction::Kind::GameModsFolder);
    }
#endif
    if (count == 0) {
        default_focus();
    }
    DrawModsFooter(snapshot, mods_note);
}

void Overlay::DrawTexturePacks(float label_width, const std::function<void()>& default_focus) {
    const ModsSnapshot snapshot = ModsCopy();
    Note("Texture packs: the first one listed wins where packs overlap.");
    // Below the list: the folder button, two options and a line about restarting.
    const char* const packs_note =
        "Packs load when the game starts. Dumps go to user/dump/textures.";
    const float footer =
        ImGui::GetFrameHeightWithSpacing() * 3.f +
        (snapshot.restart_needed ? ImGui::GetTextLineHeightWithSpacing() : NoteHeight(packs_note));
    const float height = std::max(ImGui::GetFrameHeightWithSpacing() * 2.f,
                                  ImGui::GetContentRegionAvail().y - footer);
    ImGui::BeginChild("##packs", {0.f, height}, ImGuiChildFlags_NavFlattened);
    if (snapshot.packs.empty()) {
        ImGui::TextWrapped("No texture packs yet. Put each pack in its own folder in %s",
                           snapshot.folder.c_str());
    }
    const int count = DrawModList(snapshot, &ModsSnapshot::packs, OverlayAction::Kind::TexturePack,
                                  OverlayAction::Kind::TexturePackUp,
                                  OverlayAction::Kind::TexturePackDown, default_focus);
    ImGui::EndChild();

#ifndef __ANDROID__   // msr-native's Android app: the folder above, reached from a PC over USB
    if (ImGui::Button("Open the texture packs folder", {-FLT_MIN, 0.f})) {
        Act(OverlayAction::Kind::ModsFolder);
    }
#endif
    if (count == 0) {
        default_focus();
    }
    const auto toggle = [&](const char* label, const char* id, bool value, OverlayAction::Kind kind,
                            bool ModsSnapshot::*field) {
        Label(label, label_width);
        if (ImGui::Checkbox(id, &value)) {
            ChangeMods(kind, value ? 1 : 0, [field, value](ModsSnapshot& m) { m.*field = value; });
        }
    };
    ImGui::BeginDisabled(!snapshot.texture_options);
    toggle("Preload textures", "##preload", snapshot.preload, OverlayAction::Kind::PreloadTextures,
           &ModsSnapshot::preload);
    toggle("Dump game textures", "##dump", snapshot.dump, OverlayAction::Kind::DumpTextures,
           &ModsSnapshot::dump);
    ImGui::EndDisabled();
    DrawModsFooter(snapshot, packs_note);
}

void Overlay::DrawFps(float scale, float width) {
    char label[32];
    std::snprintf(label, sizeof(label), "%.0f FPS", fps);
    const float size = 18.f * scale;
    const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, label);
    const ImVec2 pos{(width - text.x) / 2.f, 8.f * scale};
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({pos.x - 6.f * scale, pos.y - 3.f * scale},
                        {pos.x + text.x + 6.f * scale, pos.y + text.y + 3.f * scale},
                        IM_COL32(0, 0, 0, 140), 4.f * scale);
    draw->AddText(ImGui::GetFont(), size, pos, IM_COL32(220, 240, 255, 255), label);
}

void Overlay::DrawMenu(float scale, bool appearing) {
    auto& io = ImGui::GetIO();
    // Snapshots edited by the widgets; changes are published or queued below.
    PlayerSettings p;
    EmulatorSettings e;
    LauncherSnapshot l;
    {
        std::scoped_lock lock{mutex};
        p = player;
        e = emulator;
        l = launcher;
    }
    const PlayerSettings before = p;
    bool player_changed = false;
    bool emulator_changed = false;

    constexpr int TabCount = 7;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) {
        requested_tab = (tab + TabCount - 1) % TabCount;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) {
        requested_tab = (tab + 1) % TabCount;
    }

    const ImVec2 size{780.f * scale, 640.f * scale};
    ImGui::SetNextWindowPos({io.DisplaySize.x * .5f, io.DisplaySize.y * .5f}, ImGuiCond_Always,
                            {.5f, .5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(.94f);
    ImGui::Begin("Metroid: Samus Returns##settings", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar);

    // Labels take 40% of the width and wrap when they need more. Each tab is a page that
    // scrolls above the footer (a separator and a line of text) when the view is too short.
    const float label_width = ImGui::GetContentRegionAvail().x * .4f;
    const float footer =
        ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 1.f;
    const auto row = [&](const char* label) {
        Label(label, label_width);
        ImGui::SetNextItemWidth(-FLT_MIN);
    };
    const auto tab_item = [&](const char* name, int index) {
        const ImGuiTabItemFlags flags = requested_tab == index ? ImGuiTabItemFlags_SetSelected : 0;
        const bool open = ImGui::BeginTabItem(name, nullptr, flags);
        if (open) {
            tab = index;
            ImGui::BeginChild("##page", {0.f, -footer}, ImGuiChildFlags_NavFlattened);
        }
        return open;
    };
    const auto end_tab_item = [] {
        ImGui::EndChild();
        ImGui::EndTabItem();
    };

    if (ImGui::BeginTabBar("##tabs")) {
        const auto default_focus = [&] {
            if (appearing) {
                ImGui::SetItemDefaultFocus();
            }
        };
        if (tab_item("Game", 0)) {
            if (l.active) {
                DrawLauncher(l, label_width, default_focus);
                // The game reads the language when it starts: in the launcher only, as in the emulator-based 1.x
                row("Language");
                static constexpr auto names = [] {
                    std::array<const char*, Languages.size()> n{};
                    for (std::size_t i = 0; i < n.size(); ++i) n[i] = Languages[i].name;
                    return n;
                }();
                int language = static_cast<int>(
                    std::find_if(Languages.begin(), Languages.end(),
                                 [&](const Language& entry) { return entry.value == p.language; }) -
                    Languages.begin());
                if (ImGui::Combo("##language", &language, names.data(), static_cast<int>(names.size())) &&
                    language < static_cast<int>(Languages.size())) {
                    p.language = Languages[language].value;
                    player_changed = true;
                }
                Note("The game's text. German, Italian and Dutch set the 3DS region to Europe, which the game "
                     "needs for them.");
            }
            row("Volume");
            emulator_changed |= ImGui::SliderInt("##volume", &e.volume, 0, 100, "%d%%");
            row("Unlock amiibo galleries");
            player_changed |= ImGui::Checkbox("##galleries", &p.unlock_galleries);
            row("Unlock Fusion Mode");
            player_changed |= ImGui::Checkbox("##fusionmode", &p.unlock_fusion_mode);
            row("Hard Mode from the start");
            player_changed |= ImGui::Checkbox("##hardmode", &p.unlock_hard_mode);
            Note("A new file and every New Game offer Normal and Hard (Fusion too).");
            row("Fusion suit, any difficulty");
            if (Cheats::randomizer.load(std::memory_order_relaxed)) {
                // The randomizer has its own Fusion suit option (msr/cheats.h): kept off.
                bool off = false;
                ImGui::BeginDisabled();
                ImGui::Checkbox("##fusionsuit", &off);
                ImGui::EndDisabled();
                Note("Off with the randomizer. Fusion Mode: chosen when a game starts.");
            } else {
                player_changed |= ImGui::Checkbox("##fusionsuit", &p.fusion_suit);
                Note("Fusion Mode: chosen when a game starts. The suit switches at once.");
            }
            ImGui::Spacing();
            const ImVec2 button{-FLT_MIN, 0.f};
            if (l.active) {
#ifdef __ANDROID__
                // msr-native's Android app: no folders to open; a PC reaches this one over USB
                Note(("The user folder (saves, settings, game mods, texture packs, the game file and the native "
                      "code): " + l.folder + ". From a PC over USB: Android/data/io.github.gminustwo.msrnative/files/user.")
                         .c_str());
#else
                if (ImGui::Button("Open the user folder", button)) {
                    Act(OverlayAction::Kind::LauncherFolder);
                }
                Note(("Saves, settings, game mods and texture packs: " + l.folder).c_str());
#endif
            } else {
                if (ImGui::Button("Resume", button)) {
                    SetVisible(false);
                }
                default_focus();
                ImGui::BeginDisabled(!e.quick_saves);
                if (ImGui::Button("Quick save", button)) {
                    Act(OverlayAction::Kind::QuickSave);
                    SetVisible(false);
                }
                if (ImGui::Button("Quick load", button)) {
                    Act(OverlayAction::Kind::QuickLoad);
                    SetVisible(false);
                }
                ImGui::EndDisabled();
                if (ImGui::Button("Restart", button)) {
                    Act(OverlayAction::Kind::Restart);
                    SetVisible(false);
                }
            }
            if (ImGui::Button("Quit", button)) {
                Act(OverlayAction::Kind::Quit);
            }
            end_tab_item();
        }
        if (tab_item("Display", 1)) {
            if (e.deck_preset) {
                // Measured on a Steam Deck OLED with the 4x texture pack (2026-11-03): 3x, the Deck's own 720p,
                // left 40% of the GPU spare at 60 FPS (about 100 without a limit); 4x held 60 with less to spare.
                const bool in_use = e.resolution == 3 && e.vsync && p.frame_rate == 60;
                if (ImGui::Button("Steam Deck: 60 FPS with texture packs", {-FLT_MIN, 0.f})) {
                    e.resolution = 3;
                    e.vsync = true;
                    emulator_changed = true;
                    p.frame_rate = 60;
                    player_changed = true;
                }
                default_focus();
                Note(in_use ? "In use: 3x internal resolution, 60 FPS, V-Sync. For even frames, set the Deck's refresh "
                              "rate to 60 Hz (Quick Access menu, Performance)."
                            : "Sets 3x internal resolution (the Deck's 720p), 60 FPS and V-Sync. For even frames, set "
                              "the Deck's refresh rate to 60 Hz (Quick Access menu, Performance).");
                ImGui::Spacing();
            }
            // Minimum, Medium, High, Ultra, or Custom: the player's own values, brought back once a preset replaced them
            row("Video preset");
            const int preset = VideoPresetOf(p, e);
            if (ImGui::BeginCombo("##preset", preset >= 0 ? VideoPresets[preset].name : "Custom")) {
                for (int i = 0; i < static_cast<int>(VideoPresets.size()); ++i) {
                    if (ImGui::Selectable(VideoPresets[i].name, preset == i) && preset != i) {
                        ApplyVideoPreset(VideoPresets[i], p, e);
                        emulator_changed = player_changed = true;
                    }
                    if (preset == i) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::BeginDisabled(!p.custom_video);
                if (ImGui::Selectable("Custom", preset < 0) && preset >= 0) {
                    e.resolution = std::clamp(p.custom_resolution, 0, e.max_resolution);
                    if (e.aniso_option) {
                        e.aniso = AnisoChoice(p.custom_aniso);
                    }
                    if (e.performance_options) {
                        e.fsr = p.custom_fsr;
                        p.wide_culling = p.custom_wide_culling;
                        e.power_saving = p.custom_power_saving;
                    }
                    p.frame_rate = p.custom_frame_rate;
                    emulator_changed = player_changed = true;
                }
                ImGui::EndDisabled();
                if (preset < 0) {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (!e.deck_preset) {
                default_focus();
            }
            {
                const int shown = VideoPresetOf(p, e);
                Note(shown >= 0 ? VideoPresetNote(VideoPresets[shown], e).c_str()
                     : e.performance_options
                         ? "Custom: the internal resolution, anisotropic filtering, upscaling, frame rate and the "
                           "options for slower computers as you set them below. They are kept when you choose a "
                           "preset: Custom brings them back."
                         : "Custom: the internal resolution and frame rate as you set them below. They are kept when "
                           "you choose a preset: Custom brings them back.");
            }
            row("Fullscreen");
            emulator_changed |= ImGui::Checkbox("##fullscreen", &e.fullscreen);
            // Ultrawide widens the 16:9 view: on, it turns Widescreen on; Widescreen off turns it off
            row("Widescreen (16:9)");
            if (ImGui::Checkbox("##widescreen", &p.widescreen)) {
                player_changed = true;
                p.ultrawide = p.ultrawide && p.widescreen;
                p.fill_16_10 = p.fill_16_10 && p.widescreen;
            }
            if (e.ultrawide_option) {
                row("Ultrawide (up to 32:9)");
                if (ImGui::Checkbox("##ultrawide", &p.ultrawide)) {
                    player_changed = true;
                    p.widescreen = p.widescreen || p.ultrawide;
                }
                Note("In play, a window wider than 16:9 shows more of the level. It needs Widescreen "
                     "(16:9), which it turns on.");
                // #38: the same below 16:9, down to 16:10
                row("Fill 16:10 screens");
                if (ImGui::Checkbox("##fill1610", &p.fill_16_10)) {
                    player_changed = true;
                    p.widescreen = p.widescreen || p.fill_16_10;
                }
                Note("In play, a window taller than 16:9, down to 16:10 (Steam Deck, most laptops, a Mac in "
                     "fullscreen), shows more of the level above and below instead of bars. It needs Widescreen "
                     "(16:9), which it turns on.");
            }
            row("Camera zoom");
            player_changed |= ImGui::SliderFloat("##zoom", &p.camera_zoom, CameraZoomMin,
                                                 CameraZoomMax, "%.2f");
            if (e.bloom_option) {
                row("Bloom");
                int bloom = static_cast<int>(std::lround(p.bloom * 100.f));
                if (ImGui::SliderInt("##bloom", &bloom, 0, 100, "%d%%")) {
                    p.bloom = static_cast<float>(bloom) / 100.f;
                    player_changed = true;
                }
            }
            if (e.hdr_option) {
                row("HDR");
                bool hdr = p.hdr && e.hdr_option == 2;
                ImGui::BeginDisabled(e.hdr_option < 2);
                if (ImGui::Checkbox("##hdr", &hdr)) {
                    p.hdr = hdr;
                    player_changed = true;
                }
                ImGui::EndDisabled();
                if (e.hdr_option < 2) {
                    Note("Turn on Use HDR in Windows' display settings first.");
                }
                if (hdr) {
                    row("HDR highlights");
                    int peak = static_cast<int>(p.hdr_peak > 0.f ? p.hdr_peak : e.display_peak);
                    if (ImGui::SliderInt("##hdrpeak", &peak, 400, 4000, "%d nits")) {
                        p.hdr_peak = static_cast<float>(peak);
                        player_changed = true;
                    }
                    row("HDR paper white");
                    int paper = static_cast<int>(p.hdr_paper > 0.f ? p.hdr_paper : e.display_paper);
                    if (ImGui::SliderInt("##hdrpaper", &paper, 80, 500, "%d nits")) {
                        p.hdr_paper = static_cast<float>(paper);
                        player_changed = true;
                    }
                    Note("Highlights: the brightest lights and glows. Paper white: the game's white, menus and HUD.");
                }
            }
            if (e.bloom_option) {   // msr-native's picture: anti-aliasing, texture packs' filtering, colour (#51, #25)
                row("Anti-aliasing (FXAA)");
                player_changed |= ImGui::Checkbox("##fxaa", &p.fxaa);
                Note("Smooths the game's jagged edges, before the picture is scaled to the window. A higher internal "
                     "resolution smooths them too, at a higher cost.");
                row("Smooth texture packs");
                player_changed |= ImGui::Checkbox("##smoothpacks", &p.smooth_packs);
                Note("A texture pack's textures filtered smoothly, also where the game shows its own with sharp "
                     "pixels.");
                auto percent = [&](const char* label, const char* id, float& value) {
                    row(label);
                    int v = static_cast<int>(std::lround(value * 100.f));
                    if (ImGui::SliderInt(id, &v, 50, 150, "%d%%")) {
                        value = static_cast<float>(v) / 100.f;
                        player_changed = true;
                    }
                };
                percent("Brightness", "##brightness", p.brightness);
                percent("Contrast", "##contrast", p.contrast);
                percent("Saturation", "##saturation", p.saturation);
                if (p.brightness != 1.f || p.contrast != 1.f || p.saturation != 1.f) {
                    row("");
                    if (ImGui::Button("Colours as the game draws them", {ImGui::GetContentRegionAvail().x, 0.f})) {
                        p.brightness = p.contrast = p.saturation = 1.f;
                        player_changed = true;
                    }
                }
            }
            row("Internal resolution");
            const int resolutions =
                std::clamp(e.max_resolution + 1, 2, static_cast<int>(ResolutionNames.size()));
            int resolution = std::clamp(e.resolution, 0, resolutions - 1);
            if (ImGui::Combo("##resolution", &resolution, ResolutionNames.data(), resolutions)) {
                e.resolution = resolution;
                emulator_changed = true;
            }
            if (e.aniso_option) {
                row("Anisotropic filtering");
                int aniso = static_cast<int>(
                    std::find(AnisoChoices.begin(), AnisoChoices.end(), AnisoChoice(e.aniso)) - AnisoChoices.begin());
                if (ImGui::Combo("##aniso", &aniso, AnisoNames.data(), static_cast<int>(AnisoNames.size()))) {
                    e.aniso = AnisoChoices[aniso];
                    emulator_changed = true;
                }
            }
            if (e.performance_options) {
                // FSR 1 sharpens the picture to the window's size in two passes there; linear is one plain stretch
                row("Upscaling");
                static constexpr std::array<const char*, 2> upscaling{"FSR 1 (sharper)", "Linear (lighter)"};
                int filter = e.fsr ? 0 : 1;
                if (ImGui::Combo("##upscaling", &filter, upscaling.data(), static_cast<int>(upscaling.size()))) {
                    e.fsr = filter == 0;
                    emulator_changed = true;
                }
            }
            row("Frame rate");
            const auto& choices = FrameRate::Choices;
            int rate = static_cast<int>(std::find(choices.begin(), choices.end(), p.frame_rate) -
                                        choices.begin());
            if (ImGui::Combo("##framerate", &rate, FrameRateNames.data(),
                             static_cast<int>(FrameRateNames.size())) &&
                rate < static_cast<int>(choices.size())) {
                p.frame_rate = choices[rate];
                player_changed = true;
            }
            row("V-Sync");
            emulator_changed |= ImGui::Checkbox("##vsync", &e.vsync);
            row("Show FPS");
            player_changed |= ImGui::Checkbox("##fps", &p.show_fps);
            row("Menu size");
            player_changed |= ImGui::SliderFloat("##menu", &p.menu_scale, .7f, 1.5f, "%.2f");
            if (e.performance_options) {
                ImGui::Spacing();
                ImGui::SeparatorText("For slower computers");
                row("Power-saving waits");
                emulator_changed |= ImGui::Checkbox("##powersaving", &e.power_saving);
                Note("While the game waits (for the next frame, for the graphics), it sleeps instead of checking in a "
                     "loop: more of a slow processor for the game, and on a handheld or laptop more power for the "
                     "graphics. On by default with 4 processor threads or fewer.");
                // checked lightens the load, as the row above: the setting kept is wide_culling, its opposite
                row("Draw only the 3DS's view");
                bool only_3ds = !p.wide_culling;
                if (ImGui::Checkbox("##only3ds", &only_3ds)) {
                    p.wide_culling = !only_3ds;
                    player_changed = true;
                }
                Note("As in 1.x: with widescreen, ultrawide or a zoom below 1, the game draws only what the 3DS's own "
                     "screen would show, so enemies and items at the edges appear as they come within it. Fewer "
                     "draws: about 12% at 16:9, 30% at zoom 0.70, 40% in ultrawide.");
                row("Texture pack size");
                static constexpr std::array<const char*, 3> sizes{"Full", "Half (a quarter of the memory)",
                                                                  "Quarter (a sixteenth)"};
                int size = e.texture_detail >= 4 ? 2 : e.texture_detail >= 2 ? 1 : 0;
                if (ImGui::Combo("##packsize", &size, sizes.data(), static_cast<int>(sizes.size()))) {
                    e.texture_detail = size == 2 ? 4 : size == 1 ? 2 : 1;
                    emulator_changed = true;
                }
                if (!l.active && e.texture_detail != e.texture_detail_running) {
                    ImGui::TextColored({.53f, .89f, .6f, 1.f}, "Restart the game to load the packs at this size.");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Restart now##packsize")) {
                        Act(OverlayAction::Kind::Restart);
                        SetVisible(false);
                    }
                } else {
                    Note("Texture pack images are shrunk as they load, never below the game's own size: less graphics "
                         "memory and less to upload, for graphics with 2 GB or less. Applies when the game starts.");
                }
                if (e.renderer_option) {
                    row("Graphics API");
                    static constexpr std::array<const char*, 3> apis{"Automatic", "Vulkan", "OpenGL"};
                    if (ImGui::Combo("##renderer", &e.renderer, apis.data(), static_cast<int>(apis.size()))) {
                        emulator_changed = true;
                    }
                    if (!l.active && e.renderer != e.renderer_running) {
                        ImGui::TextColored({.53f, .89f, .6f, 1.f}, "Restart the game to use it.");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Restart now##renderer")) {
                            Act(OverlayAction::Kind::Restart);
                            SetVisible(false);
                        }
                    } else {
                        Note(e.renderer_active == 2
                                 ? "In use: OpenGL, for graphics without Vulkan 1.3 (no HDR with it). Automatic uses "
                                   "Vulkan where it can."
                                 : "Automatic uses Vulkan where the graphics have Vulkan 1.3, else OpenGL 3.3 (no HDR "
                                   "with it).");
                    }
                }
            }
            end_tab_item();
        }
        if (tab_item("HUD", 2)) {
            row("HUD size");
            player_changed |= ImGui::SliderFloat("##hudsize", &p.hud_scale, .6f, 1.6f, "%.2f");
            default_focus();
            row("HUD opacity");
            player_changed |= ImGui::SliderFloat("##hudopacity", &p.hud_opacity, .3f, 1.f, "%.2f");
            row("Map");
            player_changed |= ImGui::Checkbox("##map", &p.show_minimap);
            row("Map size");
            player_changed |= ImGui::SliderFloat("##mapsize", &p.minimap_size, .6f, 1.8f, "%.2f");
            row("Map opacity");
            player_changed |= ImGui::SliderFloat("##mapopacity", &p.minimap_opacity, .3f, 1.f, "%.2f");
            row("DNA and Metroid counters");
            player_changed |= ImGui::Checkbox("##counters", &p.show_counters);
            row("Aeion abilities");
            player_changed |= ImGui::Checkbox("##abilities", &p.show_abilities);
            if (e.ultrawide_option) {
                row("HUD within 16:9");
                player_changed |= ImGui::Checkbox("##hud169", &p.hud_within_16_9);
                Note("On an ultrawide screen, the HUD nearer the middle.");
            }
            row("Menu panel size");
            player_changed |= ImGui::SliderFloat("##panel", &p.panel_scale, .5f, 1.f, "%.2f");
            row("Title menu background");
            player_changed |= ImGui::SliderFloat("##backdrop", &p.menu_backdrop, 0.f, 1.f, "%.2f");
            row("Menu help text");
            player_changed |= ImGui::Checkbox("##hints", &p.control_hints);
            Note("Help text beside the pause menu and file select, and in touch mode.");
            end_tab_item();
        }
        if (tab_item("Controls", 3)) {
            DrawControls(scale, label_width, default_focus);
            end_tab_item();
        }
        if (tab_item("Mods", 4)) {
            DrawGameMods(default_focus);
            end_tab_item();
        }
        if (tab_item("Textures", 5)) {
            DrawTexturePacks(label_width, default_focus);
            end_tab_item();
        }
        if (tab_item("Cheats", 6)) {
            row("Infinite energy");
            player_changed |= ImGui::Checkbox("##energy", &p.infinite_energy);
            default_focus();
            row("Unlimited missiles");
            player_changed |= ImGui::Checkbox("##missiles", &p.infinite_missiles);
            row("Unlimited Aeion");
            player_changed |= ImGui::Checkbox("##aeion", &p.infinite_aeion);
            Note("Kept full every frame. Missiles include Super Missiles and Power Bombs. A save "
                 "keeps what you have when you save.");
            end_tab_item();
        }
        ImGui::EndTabBar();
    }
    requested_tab = -1;

    ImGui::Separator();
    const std::string help = ButtonGlyphs::Keys(l.active ? "{Choose} choose   {Tabs} tabs"
                                                         : "{Choose} choose   {Back} back   {Tabs} tabs   {Close} close");
    ImGui::TextDisabled("%s", help.c_str());
    ImGui::End();

    // Video settings that make no preset are the player's own (set by hand, the Steam Deck button, an older version's
    // settings.ini): kept, so the Video preset row's Custom brings them back after a preset replaced them
    if (VideoPresetOf(p, e) < 0 &&
        (!p.custom_video || p.custom_resolution != e.resolution || p.custom_aniso != e.aniso ||
         p.custom_fsr != e.fsr || p.custom_frame_rate != p.frame_rate ||
         p.custom_wide_culling != p.wide_culling || p.custom_power_saving != e.power_saving)) {
        p.custom_video = true;
        p.custom_resolution = e.resolution;
        p.custom_aniso = e.aniso;
        p.custom_fsr = e.fsr;
        p.custom_frame_rate = p.frame_rate;
        p.custom_wide_culling = p.wide_culling;
        p.custom_power_saving = e.power_saving;
        player_changed = true;
    }

    {
        std::scoped_lock lock{mutex};
        player = p;
        emulator = e;
    }
    if (player_changed) {
        PlayerChanged(p.widescreen != before.widescreen || p.ultrawide != before.ultrawide || p.fill_16_10 != before.fill_16_10 ||
                      p.panel_scale != before.panel_scale);
        if (p.frame_rate != before.frame_rate) {
            Act(OverlayAction::Kind::FrameRate);
        }
    }
    if (emulator_changed) {
        EmulatorChanged();
    }

    // The focus shown from the start, so Enter, Space or the pad's button works on the focused button at once
    // (ImGui hides it until a direction is pressed, and ignores activation while it is hidden).
    if (appearing) {
        ImGui::SetNavCursorVisible(true);
    }

    // Back out of the menu with B (the bottom face button) or Escape when nothing is being edited.
    if (!ImGui::IsAnyItemActive() && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                                      ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                      ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))) {
        SetVisible(false);
    }
}

void Overlay::DrawLauncher(const LauncherSnapshot& l, float label_width,
                           const std::function<void()>& default_focus) {
    using Code = LauncherSnapshot::Code;
    using Update = LauncherSnapshot::Update;
    const auto& style = ImGui::GetStyle();
    const ImVec4 good{.53f, .89f, .6f, 1.f}, bad{1.f, .55f, .45f, 1.f}, accent{.43f, .8f, .98f, 1.f};
    // A newer release (msr-native's launcher looks when it starts): put in place here, or its page
    if (l.update != Update::None) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(accent, "msr-native %s is available.", l.update_version.c_str());
        ImGui::PopTextWrapPos();
        const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.f;
        if (l.update == Update::Working) {
            if (l.update_progress >= 0.f) {
                ImGui::ProgressBar(l.update_progress, {-FLT_MIN, 0.f});
            }
        } else if (l.update_installable) {
            if (ImGui::Button(l.update == Update::Failed ? "Try again" : "Update now", {half, 0.f})) {
                Act(OverlayAction::Kind::LauncherUpdate);
            }
            ImGui::SameLine();
            if (ImGui::Button("What's new", {half, 0.f})) {
                Act(OverlayAction::Kind::LauncherUpdatePage);
            }
        } else if (ImGui::Button("Open the download page", {-FLT_MIN, 0.f})) {
            Act(OverlayAction::Kind::LauncherUpdatePage);
        }
        if (!l.update_note.empty()) {
            ImGui::PushTextWrapPos(0.f);
            if (l.update == Update::Failed) {
                ImGui::TextColored(bad, "%s", l.update_note.c_str());
            } else {
                Note(l.update_note.c_str());
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::Separator();
        ImGui::Spacing();
    }
    // Play: the first and largest button, focused when it can start the game
    ImGui::BeginDisabled(!l.can_play);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    if (ImGui::Button("Play", {-FLT_MIN, ImGui::GetFrameHeight() * 1.7f})) {
        Act(OverlayAction::Kind::LauncherPlay);
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (l.can_play) {
        default_focus();
    }
    ImGui::Spacing();

    // The game file: typed or chosen with the file dialog
    if (l.game != game_input_from) {
        game_input_from = l.game;
        std::snprintf(game_input.data(), game_input.size(), "%s", l.game.c_str());
    }
    Label("Game file", label_width);
    const float browse = l.can_browse ? ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.f : 0.f;
    ImGui::SetNextItemWidth(l.can_browse ? -(browse + style.ItemSpacing.x) : -FLT_MIN);
    ImGui::InputTextWithHint("##game", "Samus Returns (USA), a decrypted .3ds, .cci, .cxi or .zcci", game_input.data(),
                             game_input.size());
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        Act(OverlayAction::Kind::LauncherGame, 0, game_input.data());
    }
    if (!l.can_play && l.game.empty()) {
        default_focus();
    }
    if (l.can_browse) {
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            Act(OverlayAction::Kind::LauncherBrowse);
        }
    }
    if (!l.game_note.empty()) {
        ImGui::SetCursorPosX(label_width);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(bad, "%s", l.game_note.c_str());
        ImGui::PopTextWrapPos();
    }

    // The native code: the game's code recompiled from the game file, built once
    Label("Native code", label_width);
    static constexpr const char* states[] = {"Not built yet", "Ready", "Built by another version: rebuild it",
                                             "Building...", "The build failed"};
    const ImVec4 color = l.code == Code::Ready ? good : l.code == Code::Building ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : bad;
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextColored(color, "%s", states[static_cast<int>(l.code)]);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPosX(label_width);
    ImGui::BeginGroup();
    const float width = ImGui::GetContentRegionAvail().x;
    if (l.code == Code::Building) {
        if (l.progress >= 0.f) {
            ImGui::ProgressBar(l.progress, {width, 0.f});
        }
        if (ImGui::Button("Cancel", {width, 0.f})) {
            Act(OverlayAction::Kind::LauncherCancel);
        }
    } else if (!l.can_build) {
        // msr-native's Android app: the native code comes from a PC (its launcher's Build for Android)
        if (l.code != Code::Ready) {
            ImGui::PushTextWrapPos(0.f);
            Note("Build it on a PC: in msr-native for Windows or Linux, choose the same game file and click Build for "
                 "Android on its Game tab. Then copy the recomp-android folder it makes (in its user/msr folder) into "
                 "this app's user/msr folder, shown below.");
            ImGui::PopTextWrapPos();
        }
    } else {
        ImGui::BeginDisabled(l.game.empty() || !l.game_note.empty());
        if (ImGui::Button(l.code == Code::Missing ? "Build the native code" : "Rebuild the native code", {width, 0.f})) {
            Act(OverlayAction::Kind::LauncherBuild);
        }
        ImGui::EndDisabled();
        if (!l.can_play && !l.game.empty() && l.game_note.empty()) {
            default_focus();
        }
    }
    if (!l.code_note.empty()) {
        ImGui::PushTextWrapPos(0.f);
        Note(l.code_note.c_str());
        ImGui::PopTextWrapPos();
    }
    if (l.code == Code::Ready && !l.direct.empty()) {
        ImGui::PushTextWrapPos(0.f);
        Note(("To play without this menu, start " + l.direct + " in the program's folder.").c_str());
        ImGui::PopTextWrapPos();
    }
    if (l.android_build && l.code != Code::Building) {   // the Android app's native code, built here for it
        ImGui::Spacing();
        ImGui::BeginDisabled(l.game.empty() || !l.game_note.empty());
        if (ImGui::Button("Build for Android", {width, 0.f})) {
            Act(OverlayAction::Kind::LauncherBuildAndroid);
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.f);
        Note(l.android_note.empty() ? "The native code for msr-native's Android app, from the same game file: a few "
                                      "minutes, then copied to the phone or handheld."
                                    : l.android_note.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    ImGui::Spacing();

    // Saves from a 3DS (a Checkpoint or JKSM backup) or another emulator, in place of the ones here. The
    // confirmation's buttons take the IDs of the two they replace, so a pad's focus stays on the first.
    using Saves = LauncherSnapshot::Saves;
    Label("Saves", label_width);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0.f);
    {
        const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.f;
        if (l.saves == Saves::Confirm) {
            ImGui::TextColored(accent, "Import the saves in %s?", l.saves_from.c_str());
            Note(l.saves_detail.c_str());
            Note("They replace all three files here. The current ones are kept in user/msr/save-backups.");
            if (ImGui::Button("Import###saves_a", {half, 0.f})) {
                Act(OverlayAction::Kind::LauncherImportConfirm);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel###saves_b", {half, 0.f})) {
                Act(OverlayAction::Kind::LauncherImportCancel);
            }
        } else {
#ifndef __ANDROID__   // msr-native's Android app: no dialogs or folders to open (a PC copies the saves over USB)
            ImGui::BeginDisabled(l.saves == Saves::Searching);
            if (l.can_browse) {
                if (ImGui::Button("Import saves...###saves_a", {half, 0.f})) {
                    Act(OverlayAction::Kind::LauncherImportSaves);
                }
                ImGui::SameLine();
            }
            if (ImGui::Button("Open saves folder###saves_b", {l.can_browse ? half : -FLT_MIN, 0.f})) {
                Act(OverlayAction::Kind::LauncherSavesFolder);
            }
            ImGui::EndDisabled();
#endif
            if (l.saves == Saves::Done) {
                ImGui::TextColored(good, "%s", l.saves_note.c_str());
            } else if (l.saves == Saves::Failed) {
                ImGui::TextColored(bad, "%s", l.saves_note.c_str());
            } else if (l.saves == Saves::Searching) {
                Note(l.saves_note.c_str());
            } else if (l.can_browse) {
                Note("From a 3DS (a Checkpoint or JKSM backup) or another emulator (Citra, Lime3DS, Azahar): "
                     "choose the folder they are in.");
            } else {
                Note(("From a 3DS or another emulator: copy its profile0, profile1 and profile2 folders into " +
                      l.saves_folder + ".").c_str());
            }
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::Spacing();

    Note((l.version + ": the game's code runs as native code built from your own dump, on a small "
          "3DS runtime of its own. Nothing of the game comes with this program.").c_str());
    ImGui::Separator();
}

Overlay& GetOverlay() {
    static Overlay overlay;
    return overlay;
}

} // namespace MSR
