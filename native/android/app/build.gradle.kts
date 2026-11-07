// msr-native's Android app (README.md, Releases):
//   native/android> gradlew assembleRelease -Pmsr.sdl2=<SDL2 source> -Pmsr.zstd=<zstd source> -Pmsr.png=<libpng source>
// The program (native/CMakeLists.txt) is libmain.so, run by SDL's activity (MsrActivity), with libSDL2.so and
// libshaderc_shared.so (the vertex shader compiler, from the NDK's own shaderc: buildShaderc below). Nothing of
// the game is in it: the native code is built on a PC from the player's dump (Build for Android in msr-native's
// launcher there) and copied into the app's user folder with the game file.
plugins {
    id("com.android.application")
}

// msr-native's version, as the PC builds have it (native/CMakeLists.txt)
val msrVersion: String = Regex("""set\(MSR_NATIVE_VERSION "([^"]+)"\)""")
    .find(rootProject.file("../CMakeLists.txt").readText())?.groupValues?.get(1) ?: error("no MSR_NATIVE_VERSION")
val msrVersionCode: Int = msrVersion.split(".").map { it.toIntOrNull() ?: 0 }.let {
    it.getOrElse(0) { 0 } * 10000 + it.getOrElse(1) { 0 } * 100 + it.getOrElse(2) { 0 }
}

fun msrPath(name: String): String {
    val env = "MSR_" + name.uppercase()
    val value = (project.findProperty("msr.$name") as String?) ?: System.getenv(env)
        ?: error("msr.$name is needed: -Pmsr.$name=<folder> (or $env)")
    return file(value).absolutePath.replace('\\', '/')
}
val sdlSource = msrPath("sdl2")
val jobs = (project.findProperty("msr.jobs") as String?) ?: "8"   // this PC resets under all-core builds

android {
    namespace = "io.github.gminustwo.msrnative"
    compileSdk = 35
    ndkVersion = "27.3.13750724"

    defaultConfig {
        applicationId = "io.github.gminustwo.msrnative"
        minSdk = 29
        targetSdk = 35
        versionCode = msrVersionCode
        versionName = msrVersion
        ndk {
            abiFilters += listOf("arm64-v8a")   // the native code is built for it (build_module.py --target android-arm64)
        }
        externalNativeBuild {
            cmake {
                arguments(
                    "-DMSR_SDL2_SOURCE=$sdlSource",
                    "-DMSR_ZSTD_SOURCE=${msrPath("zstd")}",
                    "-DMSR_PNG_SOURCE=${msrPath("png")}",
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",   // 16 KB pages (Android 15)
                    "-DCMAKE_JOB_POOLS=msr=$jobs",
                    "-DCMAKE_JOB_POOL_COMPILE=msr",
                    "-DCMAKE_JOB_POOL_LINK=msr",
                )
                targets("msr-native")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.31.6"
        }
    }

    sourceSets {
        named("main") {
            java.srcDirs("src/main/java", "$sdlSource/android-project/app/src/main/java")   // SDL's Java side
        }
    }

    // MSR_ANDROID_KEYSTORE (+ _PASS, _ALIAS): the release key; without it the debug key
    val keystore = System.getenv("MSR_ANDROID_KEYSTORE")
    signingConfigs {
        if (keystore != null) {
            create("release") {
                storeFile = file(keystore)
                storePassword = System.getenv("MSR_ANDROID_KEYSTORE_PASS")
                keyAlias = System.getenv("MSR_ANDROID_KEY_ALIAS") ?: "msr-native"
                keyPassword = System.getenv("MSR_ANDROID_KEYSTORE_PASS")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// shaderc as the NDK has it (../shaderc/Android.mk): libshaderc_shared.so beside libmain.so, which runtime/pica_vsgen.c
// loads to compile the game's vertex shaders. Without it they ran on the GPU's interpreter, a few frames a second on a
// phone (#37). Built into build/shaderc (a minute or two the first time), again when the NDK or the makefiles change.
val shadercOut = layout.buildDirectory.dir("shaderc").get().asFile
val buildShaderc = tasks.register<Exec>("buildShaderc") {
    val ndk = androidComponents.sdkComponents.ndkDirectory
    val makefiles = file("../shaderc")
    inputs.dir(makefiles)
    inputs.property("ndk", android.ndkVersion)
    outputs.dir(File(shadercOut, "libs"))
    doFirst {
        val windows = System.getProperty("os.name").startsWith("Windows")
        commandLine(
            ndk.get().file(if (windows) "ndk-build.cmd" else "ndk-build").asFile.absolutePath,
            "NDK_PROJECT_PATH=null",
            "APP_BUILD_SCRIPT=" + File(makefiles, "Android.mk").absolutePath,
            "NDK_APPLICATION_MK=" + File(makefiles, "Application.mk").absolutePath,
            "NDK_OUT=" + File(shadercOut, "obj").absolutePath,
            "NDK_LIBS_OUT=" + File(shadercOut, "libs").absolutePath,
            "-j$jobs",
        )
    }
}
android.sourceSets.named("main") { jniLibs.srcDir(File(shadercOut, "libs")) }
tasks.named("preBuild") { dependsOn(buildShaderc) }

// msr-native-<version>-android-arm64.apk, as the release names it
android.applicationVariants.configureEach {
    outputs.configureEach {
        (this as com.android.build.gradle.internal.api.BaseVariantOutputImpl).outputFileName =
            "msr-native-$msrVersion-android-arm64${if (buildType.name == "release") "" else "-" + buildType.name}.apk"
    }
}
