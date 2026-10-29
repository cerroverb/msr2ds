# Metroid: Samus Returns game-folder frontend, linked into citra_qt.

# Dear ImGui (externals/msr-imgui, unmodified upstream) for the in-game settings overlay.
set(MSR_IMGUI_DIR "${PROJECT_SOURCE_DIR}/externals/msr-imgui")
add_library(msr_imgui STATIC
    ${MSR_IMGUI_DIR}/imgui.cpp
    ${MSR_IMGUI_DIR}/imgui_draw.cpp
    ${MSR_IMGUI_DIR}/imgui_tables.cpp
    ${MSR_IMGUI_DIR}/imgui_widgets.cpp
    ${MSR_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(msr_imgui SYSTEM PUBLIC ${MSR_IMGUI_DIR})
target_compile_definitions(msr_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_link_libraries(msr_imgui PUBLIC vulkan-headers)

add_library(msr_frontend STATIC
    bottom_classifier.h
    code_coverage.h
    controller_autoconfig.h
    defaults.cpp
    defaults.h
    draw_probe.h
    frame_rate.h
    game_folder.cpp
    game_folder.h
    input_bridge.cpp
    input_bridge.h
    mods.cpp
    mods.h
    overlay.cpp
    overlay.h
    overlay_hook.h
    presentation.h
    saves.cpp
    saves.h
    texture_packs.h
    touch_nav.cpp
    touch_nav.h
    version.h
)

create_target_directory_groups(msr_frontend)

target_compile_definitions(msr_frontend PUBLIC ENABLE_MSR_GAME_FOLDER)
target_link_libraries(msr_frontend PUBLIC citra_common Qt6::Widgets PRIVATE citra_core msr_imgui)

# SamusReturns-Direct.exe, beside SamusReturns.exe: boots straight into the game, skipping the
# launcher (SamusReturns.exe --play). Self-contained, so it needs none of the game's DLLs.
if (WIN32)
    add_executable(msr_direct WIN32 direct/direct.cpp direct/direct.rc)
    set_target_properties(msr_direct PROPERTIES OUTPUT_NAME "SamusReturns-Direct")
    if (MINGW)
        target_link_options(msr_direct PRIVATE -municode -static)
    endif()
endif()
