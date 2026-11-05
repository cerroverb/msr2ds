/* The Android app's side of the program (android.c; the app is native/android, its Java side MsrActivity). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The folder holding user/: the app's folder on the shared storage (Android/data/<app>/files), which a PC reaches over
   USB, so the game file and the native code built on the PC can be copied in; the app's private folder without it. */
const char *msr_android_root(void);
/* The native code, copied from where the player put it (user/msr/recomp-android/libmsr_aot.so) to the app's private
   folder: Android loads code only from there. The copy's path, or the original's when the copy failed. */
const char *msr_android_module(const char *path);
/* The game's process is started again by the app: with these arguments (argc 0: the launcher). Does not return. */
void msr_android_relaunch(int argc, const char *const *argv);

#ifdef __cplusplus
}
#endif
