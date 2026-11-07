# libshaderc_shared.so for the Android app (app/build.gradle.kts, buildShaderc): shaderc as the NDK has it
# (sources/third_party/shaderc, with its glslang and SPIRV-Tools), its C API exported. runtime/pica_vsgen.c loads it at
# run time to compile the game's vertex shaders; without it they run on the GPU's interpreter (shaders/pica_vs.vert),
# which a phone's GPU runs at a few frames a second (#37).
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := shaderc_shared
LOCAL_WHOLE_STATIC_LIBRARIES := shaderc
include $(BUILD_SHARED_LIBRARY)

$(call import-module,third_party/shaderc)
