// msr-native's Android app (README.md, Releases):
//   native/android> gradlew assembleRelease -Pmsr.sdl2=<SDL2 source> -Pmsr.zstd=<zstd source> -Pmsr.png=<libpng source>
// The program (native/CMakeLists.txt) is libmain.so, run by SDL's activity (MsrActivity), with libSDL2.so. Nothing of
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

// msr-native-<version>-android-arm64.apk, as the release names it
android.applicationVariants.configureEach {
    outputs.configureEach {
        (this as com.android.build.gradle.internal.api.BaseVariantOutputImpl).outputFileName =
            "msr-native-$msrVersion-android-arm64${if (buildType.name == "release") "" else "-" + buildType.name}.apk"
    }
}
