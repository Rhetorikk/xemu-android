# xemu on Android

A work-in-progress Android port of xemu targeting a Samsung Galaxy S25
Ultra (Snapdragon 8 Elite, arm64-v8a, Adreno 830) and similar hardware.

> **Status: foundation pass.** The project structure, NDK cross-compile
> toolchain, JNI bridge, Vulkan swapchain stub, touch gamepad overlay,
> and SAF file picker are all wired. The emulator does **not** boot games
> yet — the nv2a Vulkan renderer still needs to be rewired to present
> into the Android swapchain, and the build is likely to iterate on
> glib-for-Android portability until both land cleanly.

## What this pass delivers

- Gradle project under `android/` producing an `app-debug.apk` for
  `arm64-v8a` only.
- Meson cross-file at `scripts/meson-cross/android-arm64.txt.in` and a
  Gradle task that invokes meson directly (bypassing the parent
  `configure` script and providing its own `config-host.mak`).
- New `host_os == 'android'` branches in `meson.build` and
  `ui/meson.build` that swap OpenGL for Vulkan, disable libpcap and the
  desktop UI, fall back to a `subprojects/glib.wrap` for glib, and
  switch the build artifact from `executable('qemu-system-i386')` to
  `shared_module('libxemu.so')`.
- An in-tree `audio/samplerate-stub.c` that satisfies the linker for the
  MCPX APU voice processor while audio is silent.
- New native sources:
  - `ui/xemu-android.c` — JNI lifecycle entry points + JSON config parser.
  - `ui/xemu-android-display.c` — independent Vulkan swapchain that
    clears the SurfaceView, plus surface destroy/recreate handling.
  - `ui/xemu-android-jni.c` — JNI exports.
  - `ui/xemu-os-utils-android.c` — `xemu_get_os_info()` via
    `__system_property_get`.
- Android Kotlin sources under `android/app/src/main/java/app/xemu/`:
  - `LauncherActivity` — Storage Access Framework pickers for MCPX BIOS /
    flash / EEPROM / HDD / DVD, copying small files to `filesDir`.
  - `EmulatorActivity` — `SurfaceView` host that hands the surface to
    JNI, with a `TouchGamepadOverlay` painted on top.
  - `TouchGamepadOverlay` — multi-touch on-screen gamepad (face buttons,
    D-pad, dual analog sticks, LB/RB/LT/RT, Start/Back). Routes to
    `XemuNative.nativeButton` / `nativeAxis`.
  - `XemuNative` — native method declarations.
- CI workflow `.github/workflows/build-android.yml` building the APK on
  `ubuntu-22.04`, with concurrency cancel-in-progress and a build log
  artifact on failure.

## What this pass does **not** deliver

- **Booting a game.** The nv2a Vulkan renderer currently exports its
  framebuffer for OpenGL interop. Rewiring it to present into the Android
  swapchain (and replacing `ImGui_ImplOpenGL3_*` with the Vulkan ImGui
  backend) is the next pass.
- **Audio.** Stays at `noaudio.c`. SDL3 has working `aaudio`/`opensles`
  backends to wire up later.
- **Controller / touch input mapping.** JNI exports exist but are not
  routed into xemu's input layer yet.
- **Large HDD/DVD images via SAF.** Only files ≤ 8 MiB are inline-copied;
  larger images must be pushed via `adb push` to
  `/data/data/app.xemu/files/`.

## Known compile blockers (expected to iterate)

- **glib portability**: A `subprojects/glib.wrap` is in tree, but GNOME
  glib's NDK cross-build is known to need patches (gnotification,
  GNetworkMonitor, gthread internals). The first `meson compile` is
  likely to fail somewhere in glib; address each error as it surfaces.
  Alternative: vendor a prebuilt Android glib (Termux ships one).
- **Other QEMU portability**: large portions of QEMU's tree assume
  Linux desktop semantics. Expect Android-specific compile errors and
  add `#ifdef CONFIG_ANDROID` guards or `host_os == 'android'` branches
  as they appear.

## Building locally

Prerequisites:
- Android Studio Jellyfish or newer, **or** standalone:
  - JDK 17
  - Android SDK platform 35
  - Android NDK r27.2 (`27.2.12479018`) or newer
- meson ≥ 1.5, ninja, cmake ≥ 3.22

```bash
# Tell Gradle where the NDK lives
cp android/local.properties.example android/local.properties
$EDITOR android/local.properties           # set sdk.dir and ndk.dir

# One-time: bootstrap the gradle wrapper jar from a system gradle.
( cd android && gradle wrapper --gradle-version 8.11.1 )

# Build the APK
( cd android && ./gradlew :app:assembleDebug )

# Output:
ls android/app/build/outputs/apk/debug/app-debug.apk
```

To install on a connected S25 Ultra:

```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n app.xemu/.LauncherActivity
adb logcat -s xemu xemu-vk xemu-jni
```

## Verification checklist (foundation pass)

| Step | How | Pass criterion |
|---|---|---|
| V1  | `meson setup build-android --cross-file=…/android-arm64.txt` | `Host machine system: android, cpu_family: aarch64` |
| V2  | `meson compile -C build-android pixman libslirp libglslang volk SDL3-static` | each `.a` produced |
| V3  | `meson compile -C build-android glib` | `libglib-2.0.a` produced (most patching expected here) |
| V4  | `meson compile -C build-android xemu` | `libxemu.so` exists with `Java_app_xemu_*` JNI symbols |
| V5  | `./gradlew :app:assembleDebug` | `app-debug.apk` packed with `lib/arm64-v8a/libxemu.so` |
| V6  | `adb install …` | `LauncherActivity` renders, no crash |
| V7  | Pick BIOS via SAF | copied to `/data/data/app.xemu/files/bios.bin` |
| V8  | Tap **Start** | logcat shows `xemu_android_start` + non-null `ANativeWindow*` |
| V9  | (V8 continued) | logcat shows `Selected physical device: Adreno (TM) 830` and `Swapchain extent: …` |
| V10 | (V9 continued) | SurfaceView renders solid blue |
| V11 | Background+foreground app | `VK_ERROR_OUT_OF_DATE_KHR` handled, swapchain recreated, no crash |

## Roadmap

| Pass | Scope | Status |
|------|-------|--------|
| **1. Foundation** | Gradle + NDK + JNI + Vulkan stub + touch overlay + SAF | This branch |
| 2. First boot | glib patches as needed; rewire `hw/xbox/nv2a/pgraph/vk/display.c` to present into the Android swapchain; swap `ImGui_ImplOpenGL3_*` for `ImGui_ImplVulkan_*` | Pending |
| 3. Audio | Wire SDL3 AAudio backend; rework MCPX APU resampler stub | Pending |
| 4. Input wiring | Bridge JNI `nativeButton`/`nativeAxis` into `xemu-input.c`'s `bound_controllers[]` | Pending |
| 5. Large files | `ContentResolver.openFileDescriptor` → `pread64` through QEMU's block layer for HDD/DVD images | Pending |
| 6. Perf | Big-core pinning, TCG `MAP_JIT` tuning, JNI marshaling shortcuts, Adreno-specific pipeline cache tuning | Pending |

Splitting the port across passes keeps each diff reviewable and avoids
landing thousands of lines of unverified glue at once.
