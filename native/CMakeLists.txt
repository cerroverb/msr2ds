# msr-native: Samus Returns on a Woohoo! Flight School-style runtime (no emulator). The game's code is the module
# the launcher builds from the player's dump (tools/msr/recomp/build_module.py); this program contains none of it.
#   cmake -S native -B build-native -G Ninja && cmake --build build-native
# Compilers: GCC or Clang (MinGW-w64 on Windows). Needs SDL2, Vulkan headers and loader, glslc.
cmake_minimum_required(VERSION 3.16)
project(msr_native C CXX)
set(MSR_NATIVE_VERSION "2.0.4")   # the launcher shows it; the native code's module.json records it
# Where the launcher looks for newer releases (GitHub owner/repository; its releases must be public): their packages,
# as tools/package names them, update the installation in place (launcher.cpp)
set(MSR_UPDATE_REPO "gminustwo/msr2ds" CACHE STRING "GitHub repository whose releases the launcher offers as updates")
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(MSR_AOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/arm/msr_aot)
# The module is accepted only when it was compiled against the same aot_runtime.h (as src/core/CMakeLists.txt checks).
file(READ ${MSR_AOT_DIR}/aot_runtime.h MSR_AOT_RUNTIME_TEXT)
string(REPLACE "\r" "" MSR_AOT_RUNTIME_TEXT "${MSR_AOT_RUNTIME_TEXT}")
# On AArch64 (the Android app) aot_runtime.h's SSE intrinsics are a64/msr_sse_a64.h's: that header is part of the
# runtime the native code must match, as build_module.py --target android-arm64 hashes it.
set(MSR_A64 OFF)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(MSR_A64 ON)
    file(READ ${MSR_AOT_DIR}/a64/msr_sse_a64.h MSR_A64_TEXT)
    string(REPLACE "\r" "" MSR_A64_TEXT "${MSR_A64_TEXT}")
    string(APPEND MSR_AOT_RUNTIME_TEXT "${MSR_A64_TEXT}")
endif()
string(SHA256 MSR_AOT_RUNTIME_SHA256 "${MSR_AOT_RUNTIME_TEXT}")
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)   # inside the main build, src/core already depends on it
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MSR_AOT_DIR}/aot_runtime.h)
endif()

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
# SDL2 built from a source folder (static: nothing to ship), e.g. for an AppImage on a system without SDL2's headers
set(MSR_SDL2_SOURCE "" CACHE PATH "Build SDL2 statically from this source folder instead of using the system's")
if (ANDROID AND NOT MSR_SDL2_SOURCE)
    message(FATAL_ERROR "msr-native: the Android app needs MSR_SDL2_SOURCE (SDL 2's source; its Java side is the app's)")
endif()
if (MSR_SDL2_SOURCE AND ANDROID AND NOT TARGET SDL2::SDL2)
    # Android: libSDL2.so beside the program's libmain.so, as SDL's Java side (SDLActivity) loads them
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
    add_subdirectory(${MSR_SDL2_SOURCE} ${CMAKE_CURRENT_BINARY_DIR}/sdl2 EXCLUDE_FROM_ALL)
    add_library(SDL2::SDL2 ALIAS SDL2)
elseif (MSR_SDL2_SOURCE AND NOT TARGET SDL2::SDL2)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
    set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
    add_subdirectory(${MSR_SDL2_SOURCE} ${CMAKE_CURRENT_BINARY_DIR}/sdl2 EXCLUDE_FROM_ALL)
    add_library(SDL2::SDL2 ALIAS SDL2-static)
endif()
if (NOT TARGET SDL2::SDL2)   # inside the main build the system SDL2 may already be found
    find_package(SDL2 REQUIRED)
endif()
if (ANDROID AND NOT GLSLC)   # the Android app: the NDK's own (shader-tools/<host>/glslc)
    file(GLOB MSR_NDK_GLSLC ${ANDROID_NDK}/shader-tools/*/glslc ${ANDROID_NDK}/shader-tools/*/glslc.exe)
    if (MSR_NDK_GLSLC)
        list(GET MSR_NDK_GLSLC 0 MSR_NDK_GLSLC)
        set(GLSLC ${MSR_NDK_GLSLC} CACHE FILEPATH "glslc")
    endif()
endif()
find_program(GLSLC glslc)
if (NOT GLSLC)
    # glslangValidator instead (Linux build environments often have only glslang-tools): SPIR-V, then the same C
    # initializer glslc -mfmt=c writes (cmake/spv2inc.cmake)
    find_program(GLSLANG glslangValidator REQUIRED)
endif()

set(SH ${CMAKE_CURRENT_SOURCE_DIR}/runtime/shaders)
set(SHO ${CMAKE_CURRENT_BINARY_DIR}/shaders)
# out: the .inc name; src: the shader; extra: defines
function(msr_shader out src)
  if (GLSLC)
    add_custom_command(OUTPUT ${SHO}/${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${SHO}
      COMMAND ${GLSLC} --target-env=vulkan1.3 -O ${ARGN} -mfmt=c -o ${SHO}/${out} ${SH}/${src}
      DEPENDS ${SH}/${src} ${SH}/fsr/ffx_a.h ${SH}/fsr/ffx_fsr1.h)
  else()
    add_custom_command(OUTPUT ${SHO}/${out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${SHO}
      COMMAND ${GLSLANG} -V --target-env vulkan1.3 ${ARGN} -o ${SHO}/${out}.spv ${SH}/${src}
      COMMAND ${CMAKE_COMMAND} -DIN=${SHO}/${out}.spv -DOUT=${SHO}/${out} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spv2inc.cmake
      DEPENDS ${SH}/${src} ${SH}/fsr/ffx_a.h ${SH}/fsr/ffx_fsr1.h ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spv2inc.cmake)
  endif()
endfunction()
foreach(sh pica.vert pica_vs.vert pica.frag present.vert present.frag fsr_easu.frag fsr_rcas.frag msr.vert msr.frag hdr.frag)
  msr_shader(${sh}.inc ${sh})
endforeach()
msr_shader(pica_nd.frag.inc pica.frag -DNO_FRAG_DEPTH)

# OpenGL 3.3 (runtime/video_gl.c, pica_gl.c, gl_load.c): presentation and the PICA renderer where Vulkan has no 1.3 device
# (--renderer=auto, or opengl), with the shaders as GLSL text (cmake/glsl2inc.cmake) the driver compiles. Windows and Linux.
if (WIN32 OR (UNIX AND NOT APPLE AND NOT ANDROID))
    set(MSR_OPENGL_DEFAULT ON)
else()
    set(MSR_OPENGL_DEFAULT OFF)
endif()
option(MSR_OPENGL "OpenGL presentation as well as Vulkan's" ${MSR_OPENGL_DEFAULT})
set(MSR_GL_SOURCES "")
if (MSR_OPENGL)
    foreach(sh present.vert present.frag msr.vert msr.frag fsr_easu.frag fsr_rcas.frag pica.vert pica.frag pica_vs.vert)
        add_custom_command(OUTPUT ${SHO}/${sh}.glsl.inc
          COMMAND ${CMAKE_COMMAND} -E make_directory ${SHO}
          COMMAND ${CMAKE_COMMAND} -DIN=${SH}/${sh} -DOUT=${SHO}/${sh}.glsl.inc -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/glsl2inc.cmake
          DEPENDS ${SH}/${sh} ${SH}/fsr/ffx_a.h ${SH}/fsr/ffx_fsr1.h ${CMAKE_CURRENT_SOURCE_DIR}/cmake/glsl2inc.cmake)
        list(APPEND MSR_GL_SOURCES ${SHO}/${sh}.glsl.inc)
    endforeach()
    list(APPEND MSR_GL_SOURCES runtime/video_gl.c runtime/gl_load.c runtime/pica_gl.c)
endif()

set(MSR_NATIVE_SOURCES
  runtime/main.c runtime/kernel.c runtime/services.c runtime/fs.c runtime/dump.c runtime/gsp.c runtime/pica.c runtime/video.c
  runtime/dsp.c runtime/pica_sw.c runtime/input.c runtime/pica_vk.c runtime/pica_vsgen.c runtime/runtime.c
  runtime/stubs.c runtime/interp.c runtime/interp_full.c runtime/dispatch.cpp runtime/state.c runtime/vk_push.c
  runtime/msr/settings.cpp runtime/msr/patches.cpp runtime/msr/compositor.cpp runtime/msr/cheats.cpp runtime/msr/camera_fov.cpp runtime/msr/glyphs.cpp
  runtime/msr/textures.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/common/cityhash.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/msr/touch_nav.cpp
  runtime/msr/ini.cpp runtime/msr/mods.cpp runtime/msr/patch.cpp runtime/msr/layered_fs.cpp runtime/msr/overlay_host.cpp
  runtime/msr/launcher.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/msr/overlay.cpp
  ${SHO}/pica.vert.inc ${SHO}/pica_vs.vert.inc ${SHO}/pica.frag.inc
  ${SHO}/present.vert.inc ${SHO}/present.frag.inc
  ${SHO}/fsr_easu.frag.inc ${SHO}/fsr_rcas.frag.inc ${SHO}/pica_nd.frag.inc ${SHO}/msr.vert.inc ${SHO}/msr.frag.inc
  ${SHO}/hdr.frag.inc ${MSR_GL_SOURCES})
if (ANDROID)
    # The Android app (native/android): this program as libmain.so, which SDL's Java side loads and runs (SDL_main);
    # runtime/android.c is what it asks of the app's Java side (MsrActivity)
    add_library(msr-native SHARED ${MSR_NATIVE_SOURCES} runtime/android.c)
    set_target_properties(msr-native PROPERTIES OUTPUT_NAME main)
    target_link_libraries(msr-native PRIVATE android log)
    # The guest's floating point as the 3DS computes it: no fused multiply-adds (AArch64 compilers fuse by default)
    target_compile_options(msr-native PRIVATE -ffp-contract=off)
else()
    add_executable(msr-native ${MSR_NATIVE_SOURCES})
endif()
if (ANDROID OR WIN32)
    # Vulkan 1.3's functions from the device, not the Vulkan library (runtime/vk13_load.c): Android's has none of them,
    # and Windows' may be older than 1.3, where the program must still start (and render on the CPU)
    target_sources(msr-native PRIVATE runtime/vk13_load.c)
    target_compile_definitions(msr-native PRIVATE MSR_VK13_FROM_DEVICE)
endif()
if (MSR_A64)
    target_include_directories(msr-native BEFORE PRIVATE ${MSR_AOT_DIR}/a64)   # <emmintrin.h>, <xmmintrin.h>
endif()
# Samus Returns' features share src/msr's headers with the emulator-based build (msr/compat: the types they use).
target_include_directories(msr-native PRIVATE runtime/msr/compat runtime ${SHO} ${MSR_AOT_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(msr-native PRIVATE _FILE_OFFSET_BITS=64 MSR_AOT_RUNTIME_SHA256="${MSR_AOT_RUNTIME_SHA256}" MSR_NATIVE
                           MSR_NATIVE_VERSION="${MSR_NATIVE_VERSION}" MSR_UPDATE_REPO="${MSR_UPDATE_REPO}")
# Saved vertex shaders (pwr_shaders.bin) stay valid while their generator is the same: its source's hash, not the build time
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/runtime/pica_vsgen.c MSR_VSGEN_TEXT)
string(REPLACE "\r" "" MSR_VSGEN_TEXT "${MSR_VSGEN_TEXT}")
string(SHA256 MSR_VSGEN_SHA256 "${MSR_VSGEN_TEXT}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/pica_vsgen.c)
set_source_files_properties(runtime/pica_vsgen.c PROPERTIES COMPILE_DEFINITIONS MSR_VSGEN_SHA256="${MSR_VSGEN_SHA256}")
# The settings overlay is the emulator-based build's (src/msr/overlay.cpp) on Dear ImGui (externals/msr-imgui, unmodified
# upstream, MIT), its Vulkan functions loaded at run time as there.
set(MSR_IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../externals/msr-imgui)
add_library(msr_native_imgui STATIC ${MSR_IMGUI_DIR}/imgui.cpp ${MSR_IMGUI_DIR}/imgui_draw.cpp ${MSR_IMGUI_DIR}/imgui_tables.cpp
            ${MSR_IMGUI_DIR}/imgui_widgets.cpp ${MSR_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(msr_native_imgui SYSTEM PUBLIC ${MSR_IMGUI_DIR})
target_compile_definitions(msr_native_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_link_libraries(msr_native_imgui PUBLIC Vulkan::Vulkan)
if (MSR_OPENGL)   # its loader opens the system's OpenGL library itself
    target_sources(msr_native_imgui PRIVATE ${MSR_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    target_link_libraries(msr_native_imgui PUBLIC ${CMAKE_DL_LIBS})
    target_compile_definitions(msr-native PRIVATE MSR_OPENGL)
endif()
target_link_libraries(msr-native PRIVATE msr_native_imgui)
target_compile_options(msr-native PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-unused-result>)
target_link_libraries(msr-native PRIVATE Threads::Threads Vulkan::Vulkan SDL2::SDL2 m ${CMAKE_DL_LIBS})
# zstd's decoder, for compressed game files (.zcci, .zcxi: runtime/dump.c): built statically from a zstd source folder
# (MSR_ZSTD_SOURCE: the emulator-based build's externals/zstd; the Linux release, whose build system has no zstd
# headers), else the system's libzstd (MSYS2: mingw-w64-ucrt-x86_64-zstd; Linux: libzstd-dev)
set(MSR_ZSTD_SOURCE "" CACHE PATH "Build zstd's decoder statically from this source folder instead of using the system's")
if (MSR_ZSTD_SOURCE)
    file(GLOB MSR_ZSTD_FILES ${MSR_ZSTD_SOURCE}/lib/common/*.c ${MSR_ZSTD_SOURCE}/lib/decompress/*.c)
    add_library(msr_zstd STATIC ${MSR_ZSTD_FILES})
    target_include_directories(msr_zstd SYSTEM PUBLIC ${MSR_ZSTD_SOURCE}/lib)
    target_compile_definitions(msr_zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0 XXH_NAMESPACE=ZSTD_)
    set_target_properties(msr_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(msr-native PRIVATE msr_zstd)
else()
    find_path(MSR_ZSTD_INCLUDE zstd.h)
    find_library(MSR_ZSTD_LIBRARY NAMES zstd)
    if (NOT MSR_ZSTD_INCLUDE OR NOT MSR_ZSTD_LIBRARY)
        message(FATAL_ERROR "msr-native: zstd is needed (libzstd, or MSR_ZSTD_SOURCE=<a zstd source folder>)")
    endif()
    target_include_directories(msr-native SYSTEM PRIVATE ${MSR_ZSTD_INCLUDE})
    target_link_libraries(msr-native PRIVATE ${MSR_ZSTD_LIBRARY})
endif()
# Save states are compressed with zlib when it is there (it comes with libpng)
find_package(ZLIB)
if (ZLIB_FOUND)
    target_compile_definitions(msr-native PRIVATE MSR_HAVE_ZLIB)
    target_link_libraries(msr-native PRIVATE ZLIB::ZLIB)
endif()
# Texture packs need libpng (MSYS2: mingw-w64-ucrt-x86_64-libpng; Linux: libpng-dev); without it they are left out.
# MSR_PNG_SOURCE: built statically from a libpng source folder (the Android app, whose NDK has zlib but no libpng)
set(MSR_PNG_SOURCE "" CACHE PATH "Build libpng statically from this source folder instead of using the system's")
if (MSR_PNG_SOURCE AND ZLIB_FOUND AND NOT TARGET PNG::PNG)
    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    set(PNG_STATIC ON CACHE BOOL "" FORCE)
    set(PNG_TESTS OFF CACHE BOOL "" FORCE)
    set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
    set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    add_subdirectory(${MSR_PNG_SOURCE} ${CMAKE_CURRENT_BINARY_DIR}/libpng EXCLUDE_FROM_ALL)
    set_target_properties(png_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(png_static PUBLIC ${MSR_PNG_SOURCE} ${CMAKE_CURRENT_BINARY_DIR}/libpng)
    add_library(PNG::PNG ALIAS png_static)
    set(PNG_FOUND TRUE)
else()
    find_package(PNG)
endif()
if (PNG_FOUND)
    target_compile_definitions(msr-native PRIVATE MSR_HAVE_PNG)
    target_link_libraries(msr-native PRIVATE PNG::PNG)
else()
    message(STATUS "msr-native: no libpng, texture packs are left out")
endif()
if(WIN32)
  enable_language(RC)
  if(MINGW AND CMAKE_HOST_WIN32)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} --preprocessor=gcc --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED")
  endif()
  target_sources(msr-native PRIVATE runtime/win/pwr.rc runtime/win/hdr_win.c)
  set_source_files_properties(runtime/win/pwr.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/win/pwr.manifest;${CMAKE_CURRENT_SOURCE_DIR}/runtime/win/msr-native.ico")
  target_link_libraries(msr-native PRIVATE winmm shell32 dxgi dwmapi comdlg32 ole32 uuid)   # ole32, uuid: the folder dialog
  # a window program: no console opens with it (the launcher logs to user/launcher.log, the game to the file it is given)
  set_target_properties(msr-native PROPERTIES WIN32_EXECUTABLE TRUE)
  # msr-native-direct.exe (runtime/win/direct.c): the game without the launcher. It is inside msr-native.exe (resource
  # 200), and the launcher writes it beside the program once the native code is built. Windows' libraries only.
  add_executable(msr-native-direct WIN32 runtime/win/direct.c runtime/win/direct.rc)
  set_target_properties(msr-native-direct PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/direct)
  set_source_files_properties(runtime/win/direct.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/win/pwr.manifest;${CMAKE_CURRENT_SOURCE_DIR}/runtime/win/msr-native.ico")
  target_link_options(msr-native-direct PRIVATE -static -s)
  set(MSR_DIRECT_EXE ${CMAKE_CURRENT_BINARY_DIR}/direct/msr-native-direct.exe)
  configure_file(runtime/win/direct_embed.rc.in ${CMAKE_CURRENT_BINARY_DIR}/direct_embed.rc @ONLY)
  target_sources(msr-native PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/direct_embed.rc)
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/direct_embed.rc PROPERTIES OBJECT_DEPENDS ${MSR_DIRECT_EXE})
  add_dependencies(msr-native msr-native-direct)
endif()
