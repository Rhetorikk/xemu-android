import org.gradle.api.tasks.Exec
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "app.xemu"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "app.xemu"
        minSdk = 33
        targetSdk = 35
        versionCode = 1
        versionName = "0.0.1-android-foundation"

        ndk {
            // Restrict the build to arm64-v8a (the S25 Ultra and all modern
            // Android phones). Do not pair this with a `splits.abi { include
            // ... }` block - AGP rejects the combination.
            abiFilters += "arm64-v8a"
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
            isJniDebuggable = true
            isDebuggable = true
            packaging {
                jniLibs.useLegacyPackaging = false
                jniLibs.keepDebugSymbols += "**/libxemu.so"
            }
        }
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            packaging {
                jniLibs.useLegacyPackaging = false
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        buildConfig = true
    }

    packaging {
        // SDL3 ships its own libSDL3.so we stage in jniLibs.
        jniLibs {
            useLegacyPackaging = false
            pickFirsts += "**/libxemu.so"
            pickFirsts += "**/libSDL3.so"
        }
    }

    sourceSets["main"].apply {
        // The meson build stages SDL3's Java sources into this directory.
        java.srcDir(layout.buildDirectory.dir("staged-sdl3-java"))
        jniLibs.srcDir(layout.buildDirectory.dir("staged-jni"))
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.documentfile:documentfile:1.0.1")
}

// ----------------------------------------------------------------------------
// Native build orchestration
//
// Gradle's externalNativeBuild only understands CMake/ndk-build. xemu uses
// meson, so we run it via a custom task that's wired as a preBuild dep.
// ----------------------------------------------------------------------------

val ndkRoot: String = run {
    // local.properties is gradle's standard place for machine-specific paths.
    val props = Properties()
    val f = rootProject.file("local.properties")
    if (f.exists()) {
        f.inputStream().use { props.load(it) }
    }
    props.getProperty("ndk.dir")
        ?: System.getenv("XEMU_NDK_ROOT")
        ?: System.getenv("ANDROID_NDK_HOME")
        ?: System.getenv("ANDROID_NDK_ROOT")
        ?: error("Set ndk.dir in local.properties or XEMU_NDK_ROOT in env")
}

val hostTag: String = when {
    org.gradle.internal.os.OperatingSystem.current().isLinux   -> "linux-x86_64"
    org.gradle.internal.os.OperatingSystem.current().isMacOsX  -> "darwin-x86_64"
    org.gradle.internal.os.OperatingSystem.current().isWindows -> "windows-x86_64"
    else -> error("Unsupported host OS for NDK")
}

val repoRoot: File = rootDir.parentFile
val mesonBuildDir: File = layout.buildDirectory.dir("meson-android").get().asFile
val resolvedCrossFile: File = layout.buildDirectory.file("android-arm64.txt").get().asFile

val generateMesonCrossFile by tasks.registering {
    val template = file("${repoRoot}/scripts/meson-cross/android-arm64.txt.in")
    inputs.file(template)
    outputs.file(resolvedCrossFile)
    doLast {
        resolvedCrossFile.parentFile.mkdirs()
        val text = template.readText()
            .replace("@NDK@", ndkRoot)
            .replace("@HOST_TAG@", hostTag)
        resolvedCrossFile.writeText(text)
    }
}

// xemu's meson.build expects TARGET_DIRS and a few other keys to be read
// from <build>/config-host.mak (normally written by the parent `configure`
// script). We bypass `configure` for Android and provide a minimal stub.
val generateConfigHostMak by tasks.registering {
    outputs.file(File(mesonBuildDir, "config-host.mak"))
    doLast {
        val dst = File(mesonBuildDir, "config-host.mak")
        dst.parentFile.mkdirs()
        dst.writeText(
            """
            # Automatically generated for the Android build - do not modify
            SRC_PATH=${repoRoot.absolutePath}
            TARGET_DIRS=i386-softmmu
            GDB=
            SUBDIRS=
            PYTHON=python3
            MKVENV_ENSUREGROUP=python3 ${repoRoot}/python/scripts/mkvenv.py ensuregroup
            GENISOIMAGE=
            MESON=meson
            NINJA=ninja
            EXESUF=
            """.trimIndent() + "\n"
        )
    }
}

val mesonSetup by tasks.registering(Exec::class) {
    dependsOn(generateMesonCrossFile, generateConfigHostMak)
    workingDir = repoRoot
    val markerFile = File(mesonBuildDir, "build.ninja")
    inputs.file(resolvedCrossFile)
    outputs.file(markerFile)
    onlyIf { !markerFile.exists() }
    // Use the exact option names from meson_options.txt - meson is
    // strict about - vs _ in some versions.
    commandLine = listOf(
        "meson", "setup",
        mesonBuildDir.absolutePath,
        "--cross-file", resolvedCrossFile.absolutePath,
        "--buildtype=release",
        "-Ddefault_library=static",
        "-Dwerror=false",
        "-Dtools=disabled",
        "-Dmodules=disabled",
        "-Ddocs=disabled",
        "-Dvnc=disabled",
        "-Dgtk=disabled",
        "-Dcocoa=disabled",
        "-Dxkbcommon=disabled",
        "-Dcapstone=disabled",
        "-Dcurl=disabled",
        "-Dvirglrenderer=disabled",
        "-Dvirtfs=disabled",
        "-Dfuse=disabled",
        "-Dfuse_lseek=disabled",
        "-Dlibssh=disabled",
        "-Dlibnfs=disabled",
        "-Dlibiscsi=disabled",
        "-Dbrlapi=disabled",
        "-Dgnutls=disabled",
        "-Dgcrypt=disabled",
        "-Dnettle=disabled",
        "-Dauth_pam=disabled",
        "-Dcoreaudio=disabled",
        "-Ddsound=disabled",
        "-Dalsa=disabled",
        "-Dpa=disabled",
        "-Dpipewire=disabled",
        "-Djack=disabled",
        "-Dsndio=disabled",
        "-Doss=disabled",
        "-Dlinux_aio=disabled",
        "-Dlinux_io_uring=disabled",
        "-Dlibpmem=disabled",
        "-Dnuma=disabled",
        "-Dgio=disabled",
        "-Drbd=disabled",
        "-Dglusterfs=disabled",
        "-Dsmartcard=disabled",
        "-Dusb_redir=disabled",
        "-Dxen=disabled",
        "-Dseccomp=disabled",
        "-Dcap_ng=disabled",
        "-Dattr=disabled",
        "-Dlibudev=disabled",
        "-Dmpath=disabled",
        "-Dlibdaxctl=disabled",
        "-Drdma=disabled",
        "-Dbpf=disabled",
        "-Dcrypto_afalg=disabled",
        "-Dbzip2=disabled",
        "-Drust=disabled",
        "-Dplugins=false",
        "-Dvte=disabled",
        "-Dvhost_user=disabled",
        "-Dvhost_kernel=disabled",
        "-Dvhost_vdpa=disabled",
        "-Dvhost_user_blk_server=disabled",
        "-Dvhost_crypto=disabled",
        "-Dvhost_net=disabled",
        "-Dlibvduse=disabled",
        "-Dvduse_blk_export=disabled",
        "-Dl2tpv3=disabled",
        "-Dnetmap=disabled",
        "-Dvde=disabled",
        "-Dpasst=disabled",
        // target_list is not a meson option; it's read from config-host.mak's
        // TARGET_DIRS key (which generateConfigHostMak writes).
    )
}

val mesonCompile by tasks.registering(Exec::class) {
    dependsOn(mesonSetup)
    workingDir = repoRoot
    commandLine = listOf(
        "meson", "compile", "-C", mesonBuildDir.absolutePath, "xemu"
    )
}

val stageNativeLibs by tasks.registering(Copy::class) {
    dependsOn(mesonCompile)
    from(mesonBuildDir) {
        include("libxemu.so")
        include("subprojects/SDL*/libSDL3.so")
        include("subprojects/SDL*/libSDL3.so.*")
    }
    into(layout.buildDirectory.dir("staged-jni/arm64-v8a"))
    eachFile {
        path = name // flatten
    }
    includeEmptyDirs = false
}

val stageSdlJava by tasks.registering(Copy::class) {
    dependsOn(mesonSetup)
    val sdlAndroidJava = File(mesonBuildDir, "subprojects")
    from(sdlAndroidJava) {
        include("SDL*/android-project/app/src/main/java/**/*.java")
    }
    into(layout.buildDirectory.dir("staged-sdl3-java"))
    eachFile {
        // Drop the "SDL3-x.y.z/android-project/app/src/main/java/" prefix
        val idx = path.indexOf("java/")
        if (idx >= 0) path = path.substring(idx + 5)
    }
    includeEmptyDirs = false
}

tasks.named("preBuild") {
    dependsOn(stageNativeLibs, stageSdlJava)
}
