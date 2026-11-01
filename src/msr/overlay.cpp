// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>
#include <QFileInfo>
#include <QSettings>
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
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

/// A control reminder: the game's buttons by their 3DS names, shown as the game's icons name them
/// (an Xbox pad's labels, msr/button_glyphs.h); the port's own by gamepad / keyboard.
struct ControlHint {
    const char* keys;
    const char* action;
};

// The game offers the map tools, file copy and delete only by touch (its buttons do nothing
// there); touch mode reaches them.
constexpr ControlHint PauseMapHints[] = {
    {"R", "Samus page"},
    {"B", "Back to the game"},
    {"L3 / Tab", "Touch mode: scan, map pins, info, settings"},
};
constexpr ControlHint PauseSamusHints[] = {
    {"Circle Pad", "Choose an item"},
    {"A", "Read more"},
    {"L", "Map"},
    {"B", "Back to the game"},
};
constexpr ControlHint MapPinsHints[] = {
    {"L3 / Tab", "Touch mode: move the pins"},
    {"B", "Back to the map"},
};
constexpr ControlHint FileListHints[] = {
    {"A", "Open a file"},
    {"L3 / Tab", "Touch mode: copy or delete an open file"},
};
constexpr ControlHint FileOpenHints[] = {
    {"L3 / Tab", "Touch mode: copy or delete"},
    {"B", "Back to the files"},
};
constexpr ControlHint TouchHints[] = {
    {"D-pad / stick / arrows", "Move"},
    {"A / Enter", "Tap; hold to drag"},
    {"B / Esc", "Leave touch mode"},
};
constexpr ControlHint MapPinsTouchHints[] = {
    {"D-pad / stick / arrows", "Move"},
    {"Hold A / Enter on a pin", "Drag it to the map, or to the bin to remove it"},
    {"B / Esc", "Leave touch mode"},
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

} // namespace

void PlayerSettings::Load(const QString& path) {
    QSettings file(path, QSettings::IniFormat);
    widescreen = file.value(QStringLiteral("widescreen"), widescreen).toBool();
    camera_zoom = std::clamp(file.value(QStringLiteral("camera_zoom"), camera_zoom).toFloat(),
                             CameraZoomMin, CameraZoomMax);
    hud_scale = file.value(QStringLiteral("hud_scale"), hud_scale).toFloat();
    hud_opacity = file.value(QStringLiteral("hud_opacity"), hud_opacity).toFloat();
    show_minimap = file.value(QStringLiteral("show_minimap"), show_minimap).toBool();
    minimap_size = file.value(QStringLiteral("minimap_size"), minimap_size).toFloat();
    minimap_opacity = file.value(QStringLiteral("minimap_opacity"), minimap_opacity).toFloat();
    show_counters = file.value(QStringLiteral("show_counters"), show_counters).toBool();
    show_abilities = file.value(QStringLiteral("show_abilities"), show_abilities).toBool();
    panel_scale = file.value(QStringLiteral("panel_scale"), panel_scale).toFloat();
    menu_backdrop = file.value(QStringLiteral("menu_backdrop"), menu_backdrop).toFloat();
    control_hints = file.value(QStringLiteral("control_hints"), control_hints).toBool();
    menu_scale = file.value(QStringLiteral("menu_scale"), menu_scale).toFloat();
    show_fps = file.value(QStringLiteral("show_fps"), show_fps).toBool();
    frame_rate = file.value(QStringLiteral("frame_rate"), frame_rate).toInt();
    background_shaders =
        file.value(QStringLiteral("background_shaders"), background_shaders).toBool();
    if (std::find(FrameRate::Choices.begin(), FrameRate::Choices.end(), frame_rate) ==
        FrameRate::Choices.end()) {
        frame_rate = 60;
    }
    unlock_galleries = file.value(QStringLiteral("unlock_galleries"), unlock_galleries).toBool();
    unlock_fusion_mode =
        file.value(QStringLiteral("unlock_fusion_mode"), unlock_fusion_mode).toBool();
    fusion_suit = file.value(QStringLiteral("fusion_suit"), fusion_suit).toBool();
    infinite_energy = file.value(QStringLiteral("infinite_energy"), infinite_energy).toBool();
    infinite_missiles = file.value(QStringLiteral("infinite_missiles"), infinite_missiles).toBool();
    infinite_aeion = file.value(QStringLiteral("infinite_aeion"), infinite_aeion).toBool();
}

void PlayerSettings::Save(const QString& path) const {
    QSettings file(path, QSettings::IniFormat);
    file.setValue(QStringLiteral("widescreen"), widescreen);
    file.setValue(QStringLiteral("camera_zoom"), camera_zoom);
    file.setValue(QStringLiteral("hud_scale"), hud_scale);
    file.setValue(QStringLiteral("hud_opacity"), hud_opacity);
    file.setValue(QStringLiteral("show_minimap"), show_minimap);
    file.setValue(QStringLiteral("minimap_size"), minimap_size);
    file.setValue(QStringLiteral("minimap_opacity"), minimap_opacity);
    file.setValue(QStringLiteral("show_counters"), show_counters);
    file.setValue(QStringLiteral("show_abilities"), show_abilities);
    file.setValue(QStringLiteral("panel_scale"), panel_scale);
    file.setValue(QStringLiteral("menu_backdrop"), menu_backdrop);
    file.setValue(QStringLiteral("control_hints"), control_hints);
    file.setValue(QStringLiteral("menu_scale"), menu_scale);
    file.setValue(QStringLiteral("show_fps"), show_fps);
    file.setValue(QStringLiteral("frame_rate"), frame_rate);
    file.setValue(QStringLiteral("background_shaders"), background_shaders);
    file.setValue(QStringLiteral("unlock_galleries"), unlock_galleries);
    file.setValue(QStringLiteral("unlock_fusion_mode"), unlock_fusion_mode);
    file.setValue(QStringLiteral("fusion_suit"), fusion_suit);
    file.setValue(QStringLiteral("infinite_energy"), infinite_energy);
    file.setValue(QStringLiteral("infinite_missiles"), infinite_missiles);
    file.setValue(QStringLiteral("infinite_aeion"), infinite_aeion);
}

void PlayerSettings::Publish() const {
    namespace P = Presentation;
    P::widescreen = widescreen;
    P::zoom = camera_zoom;
    P::hud_scale = hud_scale;
    P::hud_opacity = hud_opacity;
    P::show_minimap = show_minimap;
    P::minimap_size = minimap_size;
    P::minimap_opacity = minimap_opacity;
    P::show_counters = show_counters;
    P::show_abilities = show_abilities;
    P::panel_scale = panel_scale;
    P::menu_backdrop = menu_backdrop;
    FrameRate::target = frame_rate;
    Cheats::enabled = (unlock_galleries ? Cheats::Galleries : 0u) |
                      (unlock_fusion_mode ? Cheats::FusionMode : 0u) |
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
    // the bottom one (Xbox A) and goes back.
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
        if (QFileInfo::exists(QString::fromUtf8(path))) {
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

void Overlay::Act(OverlayAction::Kind kind, int value) {
    std::scoped_lock lock{mutex};
    actions.push_back({kind, emulator, value});
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

void Overlay::Shutdown() {
    if (!backend_ready) {
        return;
    }
    ImGui::SetCurrentContext(context);
    ImGui_ImplVulkan_Shutdown();
    backend_ready = false;
}

void Overlay::Draw(VkCommandBuffer command, std::uint32_t width, std::uint32_t height) {
    if (!backend_ready) {
        return;
    }
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

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    DrawHint(scale, static_cast<float>(width), static_cast<float>(height));
    if (!show) {
        DrawControlHints(static_cast<float>(width), static_cast<float>(height));
    }
    if (GetPlayerSettings().show_fps) {
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
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command);
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
    const bool menu = static_cast<P::Bottom>(P::bottom.load()) == P::Bottom::Menu &&
                      P::panel_visible.load();
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
        keys.push_back(ButtonGlyphs::Rename(hint.keys, ButtonGlyphs::shown.load()));
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

    // The bindings scroll in their own region; the reference stays below them.
    constexpr const char* reference[] = {
        "Beams: ZR (right trigger)        Morph Ball: ZL (left trigger)",
        "Settings: F1 or Select        Fullscreen: F11",
        "Touch navigation: Tab, or L3 in menus",
    };
    float reference_height = 0.f;
    for (const char* line : reference) {
        reference_height += NoteHeight(line);
    }
    const float height =
        std::max(80.f * scale, ImGui::GetContentRegionAvail().y - reference_height);
    ImGui::BeginChild("##bindings", {0.f, height}, ImGuiChildFlags_NavFlattened);
    for (const auto& row : snapshot.rows) {
        ImGui::PushID(row.id);
        Label(row.label.c_str(), label_width);
        if (snapshot.capturing == row.id) {
            const char* prompt = row.id >= 100 ? "Move a stick...   (Esc cancels)"
                                               : "Press a button or key...   (Esc cancels)";
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

    for (const char* line : reference) {
        Note(line);
    }
}

ModsSnapshot Overlay::ModsCopy() {
    std::scoped_lock lock{mutex};
    return mods;
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

    if (ImGui::Button("Open the mods folder", {-FLT_MIN, 0.f})) {
        Act(OverlayAction::Kind::GameModsFolder);
    }
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

    if (ImGui::Button("Open the texture packs folder", {-FLT_MIN, 0.f})) {
        Act(OverlayAction::Kind::ModsFolder);
    }
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
    toggle("Preload textures", "##preload", snapshot.preload, OverlayAction::Kind::PreloadTextures,
           &ModsSnapshot::preload);
    toggle("Dump game textures", "##dump", snapshot.dump, OverlayAction::Kind::DumpTextures,
           &ModsSnapshot::dump);
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
    {
        std::scoped_lock lock{mutex};
        p = player;
        e = emulator;
    }
    const PlayerSettings before = p;
    bool player_changed = false;
    bool emulator_changed = false;

    constexpr int TabCount = 7;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
        requested_tab = (tab + TabCount - 1) % TabCount;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
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
            row("Volume");
            emulator_changed |= ImGui::SliderInt("##volume", &e.volume, 0, 100, "%d%%");
            row("Unlock amiibo galleries");
            player_changed |= ImGui::Checkbox("##galleries", &p.unlock_galleries);
            row("Unlock Fusion Mode");
            player_changed |= ImGui::Checkbox("##fusionmode", &p.unlock_fusion_mode);
            row("Fusion suit, any difficulty");
            if (Cheats::randomizer.load(std::memory_order_relaxed)) {
                // The randomizer has its own Fusion suit option (msr/cheats.h): kept off.
                bool off = false;
                ImGui::BeginDisabled();
                ImGui::Checkbox("##fusionsuit", &off);
                ImGui::EndDisabled();
                Note("Off with the randomizer. Fusion Mode: a save's New Game.");
            } else {
                player_changed |= ImGui::Checkbox("##fusionsuit", &p.fusion_suit);
                Note("Fusion Mode: a save's New Game. The suit switches at once.");
            }
            ImGui::Spacing();
            const ImVec2 button{-FLT_MIN, 0.f};
            if (ImGui::Button("Resume", button)) {
                SetVisible(false);
            }
            default_focus();
            if (ImGui::Button("Quick save", button)) {
                Act(OverlayAction::Kind::QuickSave);
                SetVisible(false);
            }
            if (ImGui::Button("Quick load", button)) {
                Act(OverlayAction::Kind::QuickLoad);
                SetVisible(false);
            }
            if (ImGui::Button("Restart", button)) {
                Act(OverlayAction::Kind::Restart);
                SetVisible(false);
            }
            if (ImGui::Button("Quit", button)) {
                Act(OverlayAction::Kind::Quit);
            }
            end_tab_item();
        }
        if (tab_item("Display", 1)) {
            row("Fullscreen");
            emulator_changed |= ImGui::Checkbox("##fullscreen", &e.fullscreen);
            default_focus();
            row("Widescreen (16:9)");
            player_changed |= ImGui::Checkbox("##widescreen", &p.widescreen);
            row("Camera zoom");
            player_changed |= ImGui::SliderFloat("##zoom", &p.camera_zoom, CameraZoomMin,
                                                 CameraZoomMax, "%.2f");
            row("Internal resolution");
            int resolution = std::clamp(e.resolution, 0, static_cast<int>(ResolutionNames.size()) - 1);
            if (ImGui::Combo("##resolution", &resolution, ResolutionNames.data(),
                             static_cast<int>(ResolutionNames.size()))) {
                e.resolution = resolution;
                emulator_changed = true;
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
    const std::string help = ButtonGlyphs::Rename(
        "A choose   B back   LB / RB tabs   Select or F1 close", ButtonGlyphs::shown.load());
    ImGui::TextDisabled("%s", help.c_str());
    ImGui::End();

    {
        std::scoped_lock lock{mutex};
        player = p;
        emulator = e;
    }
    if (player_changed) {
        PlayerChanged(p.widescreen != before.widescreen || p.panel_scale != before.panel_scale);
        if (p.frame_rate != before.frame_rate) {
            Act(OverlayAction::Kind::FrameRate);
        }
    }
    if (emulator_changed) {
        EmulatorChanged();
    }

    // Back out of the menu with B (the bottom face button) or Escape when nothing is being edited.
    if (!ImGui::IsAnyItemActive() && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                                      ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                      ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))) {
        SetVisible(false);
    }
}

Overlay& GetOverlay() {
    static Overlay overlay;
    return overlay;
}

} // namespace MSR
