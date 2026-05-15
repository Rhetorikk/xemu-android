# xemu on Android — Foundation Build

This is a **foundation pass** of an Android port of xemu, targeting a Samsung
Galaxy S25 Ultra (Snapdragon 8 Elite, arm64-v8a, Adreno 830). It is not yet a
playable emulator — it is the scaffolding required to get to one.

## What this pass delivers

- Gradle project under `android/` producing an `app-debug.apk` for
  `arm64-v8a` only.
- Meson cross-file at `scripts/meson-cross/android-arm64.txt.in` and a
  Gradle task that invokes meson directly (bypassing the parent `configure`
  script).
- New `host_os == 'android'` branches in `meson.build` and `ui/meson.build`
  that swap OpenGL for Vulkan, disable libpcap / libsamplerate / the
  desktop UI, and switch the build artifact from
  `executable('qemu-system-i386')` to `shared_module('libxemu.so')`.
- New native sources:
  - `ui/xemu-android.c` — JNI lifecycle entry points.
  - `ui/xemu-android-display.c` — independent Vulkan swapchain that clears
    the SurfaceView, plus surface destroy/recreate handling.
  - `ui/xemu-android-jni.c` — JNI exports.
  - `ui/xemu-os-utils-android.c` — `xemu_get_os_info()` via
    `__system_property_get`.
- Android Kotlin sources under `android/app/src/main/java/app/xemu/`:
  - `LauncherActivity` — Storage Access Framework pickers for BIOS / flash /
    EEPROM / HDD / DVD, copying the small files to `filesDir`.
  - `EmulatorActivity` — `SurfaceView` host that hands the surface to JNI.
  - `XemuNative` — native method declarations.

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

## glib portability is the main remaining blocker for first-light compile

xemu's tree pulls in glib unconditionally. There is no `glib.wrap` in
`subprojects/` and the host system's glib isn't reachable from the NDK
toolchain. Before `meson compile` will succeed end-to-end, either add a
`subprojects/glib.wrap` for GNOME glib 2.80+ (Android-portable patches
expected) or vendor a prebuilt Android glib (Termux ships one).

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

## Why a foundation pass

A full Android port of xemu is realistically months of work spanning the
nv2a Vulkan rewire, audio backend selection, touch overlay design, file
descriptor passing through QEMU's block layer, and TCG performance tuning
for x86-on-ARM64. Splitting that into clearly-scoped passes lets each
piece land cleanly without giant unreviewable diffs.
