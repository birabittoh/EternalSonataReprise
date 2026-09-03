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

    /*
     * Stable sideload signing key.
     *
     * Signing with the machine's debug keystore makes every build a different
     * signer, so installing over an existing copy fails with
     * INSTALL_FAILED_UPDATE_INCOMPATIBLE and the app has to be uninstalled
     * first, taking its saves and extracted game data with it.
     *
     * The keystore is deliberately not in the repository (see .gitignore).
     * Create one once with:
     *
     *   keytool -genkeypair -keystore android/keystore/sideload.jks \
     *     -storetype PKCS12 -storepass eternalsonata -keypass eternalsonata \
     *     -alias eternalsonata -keyalg RSA -keysize 2048 -validity 10950 \
     *     -dname "CN=Eternal Sonata Reprise Sideload"
     *
     * Path, alias and passwords are overridable through gradle properties
     * (-PsideloadKeystore=...) or ES_KEYSTORE / ES_KEYSTORE_PASSWORD /
     * ES_KEY_ALIAS / ES_KEY_PASSWORD in the environment, so CI can inject one
     * from a secret. When no keystore is found the build falls back to debug
     * signing, which is what CI does today.
     */
    // Resolved against the android/ directory, not app/, so the keystore sits
    // next to the Gradle build rather than inside the module.
    val sideloadKeystore = rootProject.file(
        (project.findProperty("sideloadKeystore") as String?)
            ?: System.getenv("ES_KEYSTORE")
            ?: "keystore/sideload.jks"
    )

    signingConfigs {
        if (sideloadKeystore.isFile) {
            create("sideload") {
                storeFile = sideloadKeystore
                storePassword = (project.findProperty("sideloadKeystorePassword") as String?)
                    ?: System.getenv("ES_KEYSTORE_PASSWORD") ?: "eternalsonata"
                keyAlias = (project.findProperty("sideloadKeyAlias") as String?)
                    ?: System.getenv("ES_KEY_ALIAS") ?: "eternalsonata"
                keyPassword = (project.findProperty("sideloadKeyPassword") as String?)
                    ?: System.getenv("ES_KEY_PASSWORD") ?: "eternalsonata"
            }
        }
    }

    // Prebuilt native libraries, staged by CI into jniLibs/arm64-v8a/.
    sourceSets["main"].jniLibs.srcDirs(
        layout.projectDirectory.dir("jniLibs")
    )

    /*
     * SDL3's Android Java glue (org.libsdl.app.*) comes from the SDK, not from
     * this repository. librexruntime.so links SDL3 statically and registers its
     * native methods against these exact classes, so a vendored copy goes stale
     * the moment the SDK updates SDL3 and the app then aborts inside
     * System.loadLibrary with a NoSuchMethodError. Taking them from the SDK the
     * .so was built against keeps the signatures in lockstep.
     *
     * Override with -PsdkJavaDir=... or ES_SDK_JAVA_DIR when the SDK lives
     * somewhere other than sdk/android-arm64.
     */
    val sdkJavaDir = rootProject.file(
        (project.findProperty("sdkJavaDir") as String?)
            ?: System.getenv("ES_SDK_JAVA_DIR")
            ?: "../sdk/android-arm64/share/rexglue/android/java"
    )
    require(sdkJavaDir.isDirectory) {
        "SDL Java glue not found at $sdkJavaDir. Fetch the Android SDK first: " +
            "python scripts/download-sdk.py sdk/android-arm64 --pinned --platform android-arm64"
    }
    sourceSets["main"].java.srcDirs(sdkJavaDir)

    // guest_shaders.bin, staged by CI into assets/.
    sourceSets["main"].assets.srcDirs(
        layout.projectDirectory.dir("assets")
    )

    buildTypes {
        release {
            isMinifyEnabled = false
            // Sideload key when one is present, otherwise debug-signed as before
            // so a fresh clone (and CI, which has no keystore) still builds.
            signingConfig = signingConfigs.findByName("sideload")
                ?: signingConfigs.getByName("debug")
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
