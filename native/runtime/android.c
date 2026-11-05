/* The Android app's side of the program (android.h). The app (native/android) is SDL's Android activity around this
 * program (libmain.so): MsrActivity passes the arguments of the process it runs, and starts the process again with
 * other ones (relaunch), so the launcher and the game each run in a process of their own, as on a PC. */
#include "android.h"
#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *msr_android_root(void) {
    static char root[4096];
    if (!root[0]) {
        const char *dir = (SDL_AndroidGetExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_WRITE)
                              ? SDL_AndroidGetExternalStoragePath() : NULL;
        if (!dir) dir = SDL_AndroidGetInternalStoragePath();
        snprintf(root, sizeof root, "%s", dir ? dir : ".");
    }
    return root;
}

const char *msr_android_module(const char *path) {
    static char copy[4096];
    const char *internal = SDL_AndroidGetInternalStoragePath();
    struct stat src, dst;
    if (!internal || stat(path, &src)) return path;
    char dir[4096], tmp[4200];
    snprintf(dir, sizeof dir, "%s/recomp", internal);
    mkdir(dir, 0700);
    snprintf(copy, sizeof copy, "%s/libmsr_aot.so", dir);
    if (!stat(copy, &dst) && dst.st_size == src.st_size && dst.st_mtime == src.st_mtime) return copy;   /* copied before */
    snprintf(tmp, sizeof tmp, "%s.new", copy);
    FILE *in = fopen(path, "rb"), *out = in ? fopen(tmp, "wb") : NULL;
    int ok = in && out;
    static char buf[1 << 20];
    for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
    if (in) { ok = ok && !ferror(in); fclose(in); }
    if (out && fclose(out)) ok = 0;
    if (ok) {
        const struct timespec times[2] = {src.st_atim, src.st_mtim};
        utimensat(AT_FDCWD, tmp, times, 0);
        chmod(tmp, 0400);   /* code loaded at run time is kept read-only (Android 14's rule for loaded code) */
        ok = !rename(tmp, copy);
    }
    if (!ok) {
        unlink(tmp);
        fprintf(stderr, "android: the native code could not be copied to %s\n", copy);
        return path;
    }
    fprintf(stderr, "android: the native code copied to %s\n", copy);
    return copy;
}

void msr_android_relaunch(int argc, const char *const *argv) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    fflush(NULL);
    if (env && activity) {
        jclass cls = (*env)->GetObjectClass(env, activity);
        jmethodID relaunch = (*env)->GetMethodID(env, cls, "relaunch", "([Ljava/lang/String;)V");
        jclass string = (*env)->FindClass(env, "java/lang/String");
        jobjectArray args = (*env)->NewObjectArray(env, argc, string, NULL);
        for (int i = 0; i < argc; i++) {
            jstring s = (*env)->NewStringUTF(env, argv[i]);
            (*env)->SetObjectArrayElement(env, args, i, s);
            (*env)->DeleteLocalRef(env, s);
        }
        if (relaunch) (*env)->CallVoidMethod(env, activity, relaunch, args);
        else fprintf(stderr, "android: MsrActivity.relaunch is missing\n");
    }
    fflush(NULL);
    _exit(0);
}
