plugins {
    id("com.android.application")
}

/*
 * This Gradle project does NOT drive CMake. The native libraries are built
 * separately by CMake + NDK in CI (or locally), then Gradle packages the
 * prebuilt .so files and the guest_shaders.bin asset into an installable APK.
 *
 * Before running Gradle, stage the files:
 *   - .so files  → android/app/jniLibs/arm64-v8a/
 *   - guest_shaders.bin → android/app/assets/
 *
 * The CI workflow handles this staging automatically.
 */

android {
    namespace = "com.birabittoh.eternalsonata"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.birabittoh.eternalsonata"
        minSdk = 28        // matches CMAKE_ANDROID_API in CMakePresets.json
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // Only arm64 is supported right now.
            abiFilters += "arm64-v8a"
        }
    }

    // Prebuilt native libraries, staged by CI into jniLibs/arm64-v8a/.
    sourceSets["main"].jniLibs.srcDirs(
        layout.projectDirectory.dir("jniLibs")
    )

    // guest_shaders.bin, staged by CI into assets/.
    sourceSets["main"].assets.srcDirs(
        layout.projectDirectory.dir("assets")
    )

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")   // debug-sign for now
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        // Avoid stripping the prebuilt libraries — they are already stripped
        // (release build) or we want symbols for debugging (debug build).
        jniLibs.keepDebugSymbols += "**/*.so"
        // Extract native libs at install time so dlopen() can find siblings
        // by path. Equivalent to android:extractNativeLibs="true" but set
        // through the build system as the AGP lint recommends.
        jniLibs.useLegacyPackaging = true
    }
}
