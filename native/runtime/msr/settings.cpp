// The player's settings for msr-native: the emulator-based build's PlayerSettings (src/msr/overlay.h) in the same file
// and keys (user/msr/settings.ini [General], written by the launcher and the overlay), held by the same overlay
// (overlay_host.cpp) and published to the same shared state (src/msr/presentation.h, cheats.h, frame_rate.h).
#include <algorithm>
#include <cstdio>
#include <string>
#include "ini.h"
#include "msr.h"
#include "msr/cheats.h"
#include "msr/frame_rate.h"
#include "msr/overlay.h"
#include "msr/presentation.h"

namespace MSR {

void PlayerSettings::Load(const std::string& path) {
    const MsrIni::File file(path);
    const auto flag = [&](const char* key, bool& value) { value = file.Bool("General", key, value); };
    const auto number = [&](const char* key, float& value) {
        value = static_cast<float>(file.Number("General", key, value));
    };
    flag("widescreen", widescreen);
    flag("ultrawide", ultrawide);
    ultrawide = ultrawide && widescreen;   // it widens the 16:9 view: never on without it (the Display tab keeps to that)
    number("camera_zoom", camera_zoom);
    camera_zoom = std::clamp(camera_zoom, CameraZoomMin, CameraZoomMax);
    flag("wide_culling", wide_culling);
    number("hud_scale", hud_scale);
    number("hud_opacity", hud_opacity);
    flag("show_minimap", show_minimap);
    number("minimap_size", minimap_size);
    number("minimap_opacity", minimap_opacity);
    flag("show_counters", show_counters);
    flag("show_abilities", show_abilities);
    flag("hud_within_16_9", hud_within_16_9);
    number("panel_scale", panel_scale);
    number("menu_backdrop", menu_backdrop);
    flag("control_hints", control_hints);
    number("menu_scale", menu_scale);
    flag("show_fps", show_fps);
    frame_rate = static_cast<int>(file.Number("General", "frame_rate", frame_rate));
    if (std::find(FrameRate::Choices.begin(), FrameRate::Choices.end(), frame_rate) == FrameRate::Choices.end()) {
        frame_rate = 60;
    }
    number("bloom", bloom);
    bloom = std::clamp(bloom, 0.f, 1.f);
    flag("hdr", hdr);
    number("hdr_peak", hdr_peak);
    number("hdr_paper", hdr_paper);
    flag("unlock_galleries", unlock_galleries);
    flag("unlock_fusion_mode", unlock_fusion_mode);
    flag("unlock_hard_mode", unlock_hard_mode);
    flag("fusion_suit", fusion_suit);
    flag("infinite_energy", infinite_energy);
    flag("infinite_missiles", infinite_missiles);
    flag("infinite_aeion", infinite_aeion);
    language = static_cast<int>(file.Number("General", "language", language));
    if (std::none_of(Languages.begin(), Languages.end(), [&](const Language& l) { return l.value == language; })) {
        language = 1;   // English
    }
    flag("custom_video", custom_video);
    custom_resolution = std::clamp(static_cast<int>(file.Number("General", "custom_resolution", custom_resolution)), 0, 10);
    custom_aniso = AnisoChoice(static_cast<int>(file.Number("General", "custom_aniso", custom_aniso)));
    flag("custom_fsr", custom_fsr);
    flag("custom_wide_culling", custom_wide_culling);
    flag("custom_power_saving", custom_power_saving);
    custom_frame_rate = static_cast<int>(file.Number("General", "custom_frame_rate", custom_frame_rate));
    if (std::find(FrameRate::Choices.begin(), FrameRate::Choices.end(), custom_frame_rate) == FrameRate::Choices.end()) {
        custom_frame_rate = 60;
    }
}

void PlayerSettings::Save(const std::string& path) const {
    MsrIni::File file(path);
    const auto flag = [&](const char* key, bool value) { file.SetBool("General", key, value); };
    const auto number = [&](const char* key, double value) { file.SetNumber("General", key, value); };
    flag("widescreen", widescreen);
    flag("ultrawide", ultrawide);
    number("camera_zoom", camera_zoom);
    flag("wide_culling", wide_culling);
    number("hud_scale", hud_scale);
    number("hud_opacity", hud_opacity);
    flag("show_minimap", show_minimap);
    number("minimap_size", minimap_size);
    number("minimap_opacity", minimap_opacity);
    flag("show_counters", show_counters);
    flag("show_abilities", show_abilities);
    flag("hud_within_16_9", hud_within_16_9);
    number("panel_scale", panel_scale);
    number("menu_backdrop", menu_backdrop);
    flag("control_hints", control_hints);
    number("menu_scale", menu_scale);
    flag("show_fps", show_fps);
    number("frame_rate", frame_rate);
    number("bloom", bloom);
    flag("hdr", hdr);
    number("hdr_peak", hdr_peak);
    number("hdr_paper", hdr_paper);
    flag("unlock_galleries", unlock_galleries);
    flag("unlock_fusion_mode", unlock_fusion_mode);
    flag("unlock_hard_mode", unlock_hard_mode);
    flag("fusion_suit", fusion_suit);
    flag("infinite_energy", infinite_energy);
    flag("infinite_missiles", infinite_missiles);
    flag("infinite_aeion", infinite_aeion);
    number("language", language);
    flag("custom_video", custom_video);
    number("custom_resolution", custom_resolution);
    number("custom_aniso", custom_aniso);
    flag("custom_fsr", custom_fsr);
    number("custom_frame_rate", custom_frame_rate);
    flag("custom_wide_culling", custom_wide_culling);
    flag("custom_power_saving", custom_power_saving);
    if (!file.Save()) std::fprintf(stderr, "msr: could not write %s\n", path.c_str());
}

} // namespace MSR

namespace {
std::string settings_path;
}

extern "C" int msr_settings_load(const char* path) {
    settings_path = path ? path : "";
    std::FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (f) std::fclose(f);
    MSR::PlayerSettings p;
    if (f) p.Load(settings_path);
    MSR::GetOverlay().SetPlayerSettings(p);   // publishes them (presentation, extras, frame rate)
    std::fprintf(stderr, "msr: settings %s%s (extras %#x)\n", path ? path : "(none)", f ? "" : " not found, defaults",
                 MSR::Cheats::enabled.load());
    return f ? 0 : -1;
}

extern "C" int msr_settings_frame_rate(void) {
    return MSR::GetOverlay().GetPlayerSettings().frame_rate;
}

extern "C" int msr_settings_wide_culling(void) {
    return MSR::GetOverlay().GetPlayerSettings().wide_culling;
}

extern "C" int msr_settings_language(void) {
    return MSR::GetOverlay().GetPlayerSettings().language;
}

extern "C" int msr_settings_region(void) {
    const int language = msr_settings_language();
    for (const auto& l : MSR::Languages) {
        if (l.value == language) return l.european ? 2 : 1;   // Europe, else the game's own (USA)
    }
    return 1;
}

extern "C" int msr_settings_save(void) {
    if (settings_path.empty()) return -1;
    MSR::GetOverlay().GetPlayerSettings().Save(settings_path);
    return 0;
}

/// The renderer's view of the player's picture settings (pica_vk.c, video.c).
extern "C" float msr_bloom(void) {
    return MSR::Presentation::bloom.load(std::memory_order_relaxed);
}

extern "C" int msr_hdr(float* peak, float* paper) {
    namespace P = MSR::Presentation;
    *peak = P::hdr_peak.load(std::memory_order_relaxed);
    *paper = P::hdr_paper.load(std::memory_order_relaxed);
    return P::hdr.load(std::memory_order_relaxed);
}

extern "C" const char* msr_settings_file(void) {
    return settings_path.c_str();
}
