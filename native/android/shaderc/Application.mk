# libshaderc_shared.so (Android.mk): for the app's one ABI and its lowest Android, with libc++ inside as libmain.so has it
APP_ABI := arm64-v8a
APP_PLATFORM := android-29
APP_STL := c++_static
APP_OPTIM := release
APP_MODULES := shaderc_shared
APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true
